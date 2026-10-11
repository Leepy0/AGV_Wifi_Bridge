// AGV Wi-Fi 브리지 벤치 테스트용 UDP 서버 (.NET 10 file-based app)
// 실행: dotnet run BridgeTest.cs
// 명령: list | send <agv> <data> | ping <agv> [횟수] | ota <agv> <bin 파일|url> | cfg <agv> [명령]
//       | info <agv> | dspota <agv> stage <dspfw 파일|url> [apply] | dspota <agv> apply [recover] | hb | quit
//   data 표기: <STX> <ETX> <CR> <LF> \xHH 사용 가능 (공백 유지)
//   예) send 7 <STX>W1107C001X<ETX>
//   ota 7 build\agv_wifi_bridge.bin  → 이 PC 가 HTTP(8070) 로 파일을 서비스하고 ESP 에 URL 전달
//   cfg 7 set ip 192.168.10.107       → 원격 설정 (콘솔과 같은 키). cfg 7 = 현재 설정, cfg 7 save / cfg 7 reboot
//   dspota 7 stage app.dspfw apply    → 메인보드 펌웨어: 이 PC 가 HTTP 로 파일 서비스, 브리지가 받아 두고(스테이징) 바로 적용
//   dspota 7 apply                    → 스테이징된 이미지 적용 (AGV 정지 상태에서)
//   dspota 7 apply recover            → 구조 적용: 기동 불능 앱 복구 (브리지가 HELLO 를 50ms 간격으로 계속 보내는 동안
//                                       메인보드 전원을 다시 넣으면 부트로더의 부팅 직후 HELLO 창으로 진입)
//   info 7                            → 장치 정보 (ESP/메인보드 버전, 스테이징 상태)
using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Text;

const int MainPort = 50000;   // ESP server_port (MAIN_UP / STATUS / PONG 수신, 하향 송신)
const int RfidPort = 50001;   // ESP rfid_port (RFID 브로드캐스트 수신)
const int HttpPort = 8070;    // OTA 펌웨어 파일 서비스용

var agvs = new ConcurrentDictionary<byte, Agv>();
var logLock = new object();
var showHb = false;
var dspJob = 0;        // 테스트용 작업 번호 (관리 서버에서는 DB 의 job id)

using var mainSock = OpenUdp(MainPort);
using var rfidSock = OpenUdp(RfidPort);
using var cts = new CancellationTokenSource();
var rxMain = RxLoop(mainSock, cts.Token);
var rxRfid = RxLoop(rfidSock, cts.Token);

Log($"listening udp {MainPort} (main/status/pong), {RfidPort} (rfid)");
Log("commands: list | send <agv> <data> | ping <agv> [count] | ota <agv> <file.bin|url> | cfg <agv> [cmd] | info <agv> | dspota <agv> stage <file|url> [apply] | dspota <agv> apply [recover] | hb | quit");

while (Console.ReadLine() is { } line)
{
    var p = line.Trim().Split(' ', 3, StringSplitOptions.RemoveEmptyEntries);
    if (p.Length == 0) continue;
    switch (p[0].ToLowerInvariant())
    {
        case "quit":
        case "exit":
            cts.Cancel();
            return;

        case "hb":
            showHb = !showHb;
            Log($"status print {(showHb ? "on" : "off")}");
            break;

        case "list":
            foreach (var a in agvs.Values.OrderBy(a => a.Id))
                Log($"[{a.Id:D2}] {a.Ep} last={(DateTime.Now - a.LastSeen).TotalSeconds:F1}s ago {a.Status}");
            break;

        case "send" when p.Length == 3 && TryGetAgv(p[1], out var ag):
        {
            var data = ParseData(p[2]);
            var pkt = Proto.Build(Proto.MainDown, ag.Id, ag.NextDownSeq(), data);
            mainSock.Send(pkt, pkt.Length, ag.Ep);
            Log($"[{ag.Id:D2}] MAIN< {Fmt(data)}");
            break;
        }

        case "ping" when p.Length >= 2 && TryGetAgv(p[1], out var ag):
        {
            int count = p.Length == 3 && int.TryParse(p[2], out var c) ? c : 10;
            ag.Rtt.Clear();
            for (ushort i = 0; i < count; i++)
            {
                var ts = new byte[8];
                BinaryPrimitives.WriteInt64LittleEndian(ts, Stopwatch.GetTimestamp());
                var pkt = Proto.Build(Proto.Ping, ag.Id, i, ts);
                mainSock.Send(pkt, pkt.Length, ag.Ep);
                await Task.Delay(100);
            }
            await Task.Delay(500);
            var r = ag.Rtt.ToArray();
            Log(r.Length == 0
                ? $"[{ag.Id:D2}] ping: 0/{count} received"
                : $"[{ag.Id:D2}] ping: {r.Length}/{count} received, loss {100.0 * (count - r.Length) / count:F0}%, " +
                  $"rtt min {r.Min():F1} / avg {r.Average():F1} / max {r.Max():F1} ms");
            break;
        }

        case "ota" when p.Length == 3 && TryGetAgv(p[1], out var ag):
        {
            string url;
            if (p[2].StartsWith("http", StringComparison.OrdinalIgnoreCase))
            {
                url = p[2];
            }
            else
            {
                if (!File.Exists(p[2])) { Log($"file not found: {p[2]}"); break; }
                FwServer.Serve(File.ReadAllBytes(p[2]), HttpPort, Log);
                url = $"http://{LocalIpToward(ag.Ep!)}:{HttpPort}/fw.bin";
            }
            var pkt = Proto.Build(Proto.OtaStart, ag.Id, ag.NextDownSeq(), Encoding.ASCII.GetBytes(url));
            mainSock.Send(pkt, pkt.Length, ag.Ep);
            Log($"[{ag.Id:D2}] OTA_START {url}");
            break;
        }

        case "info" when p.Length == 2 && TryGetAgv(p[1], out var ag):
        {
            var pkt = Proto.Build(Proto.InfoReq, ag.Id, ag.NextDownSeq(), []);
            mainSock.Send(pkt, pkt.Length, ag.Ep);
            break;
        }

        case "dspota" when p.Length >= 3 && TryGetAgv(p[1], out var ag):
        {
            // dspota <agv> stage <file|url> [apply] | dspota <agv> apply [recover]
            var sub = p[2].Split(' ', 3, StringSplitOptions.RemoveEmptyEntries);
            var job = ++dspJob;
            string json;
            if (sub[0] == "apply")
            {
                var recover = sub.Length >= 2 && sub[1] == "recover";
                json = recover ? $"{{\"job\":{job},\"recover\":1}}" : $"{{\"job\":{job}}}";
                if (recover) Log($"[{ag.Id:D2}] 구조 적용: 브리지가 HELLO 를 반복 송신하는 동안 메인보드 전원을 껐다 켜세요");
                var pkt = Proto.Build(Proto.DspOtaApply, ag.Id, ag.NextDownSeq(), Encoding.ASCII.GetBytes(json));
                mainSock.Send(pkt, pkt.Length, ag.Ep);
                Log($"[{ag.Id:D2}] DSP_OTA_APPLY {json}");
            }
            else if (sub[0] == "stage" && sub.Length >= 2)
            {
                string url, ver = "", extra = "";
                var apply = sub.Length == 3 && sub[2] == "apply" ? 1 : 0;
                if (sub[1].StartsWith("http", StringComparison.OrdinalIgnoreCase))
                {
                    url = sub[1];
                }
                else
                {
                    if (!File.Exists(sub[1])) { Log($"file not found: {sub[1]}"); break; }
                    var data = File.ReadAllBytes(sub[1]);
                    if (data.Length < 32 || Encoding.ASCII.GetString(data, 0, 7) != "AGVDSP1") { Log("not a .dspfw file"); break; }
                    ver = Encoding.ASCII.GetString(data, 16, 16).TrimEnd('\0');
                    var crc = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(12));   // 파일 헤더의 CRC (브리지가 대조)
                    FwServer.Serve(data, HttpPort, Log);
                    url = $"http://{LocalIpToward(ag.Ep!)}:{HttpPort}/fw.dspfw";
                    extra = $",\"crc32\":{crc},\"size\":{data.Length}";
                }
                json = $"{{\"url\":\"{url}\",\"ver\":\"{ver}\",\"job\":{job},\"apply\":{apply}{extra}}}";
                var pkt = Proto.Build(Proto.DspOtaStart, ag.Id, ag.NextDownSeq(), Encoding.ASCII.GetBytes(json));
                mainSock.Send(pkt, pkt.Length, ag.Ep);
                Log($"[{ag.Id:D2}] DSP_OTA_START {json}");
            }
            else
            {
                Log("usage: dspota <agv> stage <file|url> [apply] | dspota <agv> apply [recover]");
            }
            break;
        }

        case "cfg" when p.Length >= 2 && TryGetAgv(p[1], out var ag):
        {
            // cfg | set <key> <value> | save | reboot | setup [on|off]
            var cmd = p.Length == 3 ? p[2] : "cfg";
            var pkt = Proto.Build(Proto.CfgCmd, ag.Id, ag.NextDownSeq(), Encoding.UTF8.GetBytes(cmd));
            mainSock.Send(pkt, pkt.Length, ag.Ep);
            Log($"[{ag.Id:D2}] CFG< {cmd}");
            break;
        }

        default:
            Log("unknown command or AGV not seen yet");
            break;
    }
}

// ───────── 수신 ─────────
async Task RxLoop(UdpClient u, CancellationToken ct)
{
    while (!ct.IsCancellationRequested)
    {
        UdpReceiveResult r;
        try { r = await u.ReceiveAsync(ct); }
        catch (OperationCanceledException) { break; }
        catch (SocketException) { continue; }
        Handle(r.Buffer, r.RemoteEndPoint);
    }
}

void Handle(byte[] b, IPEndPoint ep)
{
    if (!Proto.TryParse(b, out var h, out var pl))
    {
        Log($"BAD packet from {ep} ({b.Length}B)");
        return;
    }
    var a = agvs.GetOrAdd(h.Agv, id => new Agv(id));
    a.Ep = ep;                      // 하향 송신 대상 = 마지막 수신 endpoint (ESP local_port)
    a.LastSeen = DateTime.Now;

    switch (h.Type)
    {
        case Proto.MainUp:
            Log($"[{h.Agv:D2}] MAIN> {Fmt(pl)}{a.CheckSeq(h.Type, h.Seq)}");
            break;
        case Proto.Rfid:
            Log($"[{h.Agv:D2}] RFID> {Fmt(pl)}{a.CheckSeq(h.Type, h.Seq)}");
            break;
        case Proto.Status:
        {
            a.Status = Encoding.ASCII.GetString(pl);
            var gap = a.CheckSeq(h.Type, h.Seq);
            if (showHb || gap.Length > 0) Log($"[{h.Agv:D2}] STAT {a.Status}{gap}");
            break;
        }
        case Proto.OtaResult:
            Log($"[{h.Agv:D2}] OTA  {Encoding.ASCII.GetString(pl)}");
            break;
        case Proto.DspOtaResult:
            Log($"[{h.Agv:D2}] DSP  {Encoding.ASCII.GetString(pl)}");
            break;
        case Proto.Info:
            Log($"[{h.Agv:D2}] INFO {Encoding.ASCII.GetString(pl)}");
            break;
        case Proto.CfgResult:
            foreach (var l in Encoding.UTF8.GetString(pl).TrimEnd('\n').Split('\n'))
                Log($"[{h.Agv:D2}] CFG> {l}");
            break;
        case Proto.Pong when pl.Length == 8:
        {
            var ms = Stopwatch.GetElapsedTime(BinaryPrimitives.ReadInt64LittleEndian(pl)).TotalMilliseconds;
            a.Rtt.Enqueue(ms);
            Log($"[{h.Agv:D2}] PONG seq={h.Seq} rtt={ms:F1} ms");
            break;
        }
        default:
            Log($"[{h.Agv:D2}] type=0x{h.Type:X2} {Fmt(pl)}");
            break;
    }
}

// ───────── 유틸 ─────────
bool TryGetAgv(string s, out Agv a)
{
    a = null!;
    return byte.TryParse(s, out var id) && agvs.TryGetValue(id, out a!) && a.Ep is not null;
}

void Log(string msg)
{
    lock (logLock) Console.WriteLine($"{DateTime.Now:HH:mm:ss.fff} {msg}");
}

// AGV 쪽으로 나가는 인터페이스의 로컬 IP (NIC 가 여러 개일 때 OTA URL 용)
static IPAddress LocalIpToward(IPEndPoint remote)
{
    using var s = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
    s.Connect(remote);
    return ((IPEndPoint)s.LocalEndPoint!).Address;
}

static UdpClient OpenUdp(int port)
{
    var u = new UdpClient(new IPEndPoint(IPAddress.Any, port));
    // Windows: ICMP port unreachable 수신 시 ReceiveAsync 예외(10054) 방지
    if (OperatingSystem.IsWindows()) u.Client.IOControl(-1744830452, [0], null);
    return u;
}

// <STX> <ETX> <CR> <LF> \xHH → 바이트
static byte[] ParseData(string s)
{
    var o = new List<byte>();
    for (int i = 0; i < s.Length; i++)
    {
        if (s[i] == '<')
        {
            int j = s.IndexOf('>', i);
            byte? v = j < 0 ? null : s[(i + 1)..j].ToUpperInvariant() switch
            {
                "STX" => 0x02, "ETX" => 0x03, "CR" => 0x0D, "LF" => 0x0A, _ => null
            };
            if (v is byte t) { o.Add(t); i = j; continue; }
        }
        if (s[i] == '\\' && i + 3 < s.Length && s[i + 1] == 'x' &&
            byte.TryParse(s.AsSpan(i + 2, 2), NumberStyles.HexNumber, null, out var hx))
        {
            o.Add(hx); i += 3; continue;
        }
        o.Add((byte)s[i]);
    }
    return [.. o];
}

// 표시용: 제어문자는 <STX>/\xHH, 뒤에 hex 덤프
static string Fmt(byte[] d)
{
    var sb = new StringBuilder();
    foreach (var c in d)
        sb.Append(c switch
        {
            0x02 => "<STX>", 0x03 => "<ETX>", 0x0D => "<CR>", 0x0A => "<LF>",
            >= 0x20 and < 0x7F => ((char)c).ToString(),
            _ => $"\\x{c:X2}"
        });
    return $"{sb}  ({d.Length}B: {Convert.ToHexString(d)})";
}

// ───────── OTA 파일 서비스: 최소 HTTP 서버 (GET 전용, 마지막 Serve 호출 파일 제공) ─────────
static class FwServer
{
    static TcpListener? _l;
    static byte[] _data = [];

    public static void Serve(byte[] data, int port, Action<string> log)
    {
        _data = data;
        if (_l is not null) return;
        _l = new TcpListener(IPAddress.Any, port);
        _l.Start();
        log($"http file server on :{port} ({data.Length} bytes)");
        _ = Task.Run(async () =>
        {
            while (true)
            {
                var c = await _l.AcceptTcpClientAsync();
                _ = Task.Run(() => HandleClient(c, log));
            }
        });
    }

    static async Task HandleClient(TcpClient c, Action<string> log)
    {
        using (c)
        {
            c.NoDelay = true;
            var ns = c.GetStream();
            var buf = new byte[2048];
            var req = new StringBuilder();
            // 요청 헤더 끝(빈 줄)까지만 읽음
            while (!req.ToString().Contains("\r\n\r\n"))
            {
                int n = await ns.ReadAsync(buf);
                if (n <= 0) return;
                req.Append(Encoding.ASCII.GetString(buf, 0, n));
            }
            var data = _data;
            log($"http {c.Client.RemoteEndPoint} {req.ToString().Split("\r\n")[0]} -> {data.Length} bytes");
            var hdr = $"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: {data.Length}\r\nConnection: close\r\n\r\n";
            await ns.WriteAsync(Encoding.ASCII.GetBytes(hdr));
            await ns.WriteAsync(data);
            await ns.FlushAsync();
        }
    }
}

// ───────── 패킷 규격 (펌웨어 proto.h 와 동일) ─────────
static class Proto
{
    public const byte MainUp = 0x01, MainDown = 0x02, Rfid = 0x03, Status = 0x10, Ping = 0x20, Pong = 0x21,
                      OtaStart = 0x30, OtaResult = 0x31, CfgCmd = 0x40, CfgResult = 0x41,
                      InfoReq = 0x11, Info = 0x12, DspOtaStart = 0x32, DspOtaResult = 0x33, DspOtaApply = 0x34;
    const byte Magic0 = 0xA5, Magic1 = 0x5A, Ver = 1;
    const int HdrLen = 10;

    public static byte[] Build(byte type, byte agv, ushort seq, ReadOnlySpan<byte> payload)
    {
        var b = new byte[HdrLen + payload.Length];
        b[0] = Magic0; b[1] = Magic1; b[2] = Ver; b[3] = type; b[4] = agv; b[5] = 0;
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(6), seq);
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(8), (ushort)payload.Length);
        payload.CopyTo(b.AsSpan(HdrLen));
        return b;
    }

    public static bool TryParse(byte[] b, out Hdr h, out byte[] payload)
    {
        h = default; payload = [];
        if (b.Length < HdrLen || b[0] != Magic0 || b[1] != Magic1 || b[2] != Ver) return false;
        var len = BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(8));
        if (len != b.Length - HdrLen) return false;
        h = new Hdr(b[3], b[4], BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(6)), len);
        payload = b[HdrLen..];
        return true;
    }
}

readonly record struct Hdr(byte Type, byte Agv, ushort Seq, ushort Len);

sealed class Agv(byte id)
{
    public byte Id { get; } = id;
    public IPEndPoint? Ep;
    public DateTime LastSeen;
    public string Status = "";
    public readonly ConcurrentQueue<double> Rtt = new();
    readonly Dictionary<byte, ushort> _lastSeq = new();
    int _downSeq;

    public ushort NextDownSeq() => (ushort)Interlocked.Increment(ref _downSeq);

    // 타입별 seq 연속성 검사 → 누락 표시
    public string CheckSeq(byte type, ushort seq)
    {
        lock (_lastSeq)
        {
            var r = "";
            if (_lastSeq.TryGetValue(type, out var last))
            {
                var expect = (ushort)(last + 1);
                if (seq != expect) r = seq == 0 ? "  [SEQ RESET]" : $"  [LOSS {(ushort)(seq - expect)}]";
            }
            _lastSeq[type] = seq;
            return r;
        }
    }
}
