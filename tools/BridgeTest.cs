// AGV Wi-Fi 브리지 벤치 테스트용 UDP 서버 (.NET 10 file-based app)
// 실행: dotnet run BridgeTest.cs
// 명령: list | send <agv> <data> | ping <agv> [횟수] | ota <agv> <bin 파일|url> | cfg <agv> [명령]
//       | info <agv> | dspota <agv> stage <dsppkg 파일> [apply] | dspota <agv> apply [recover] [job] | hb | quit
//   data 표기: <STX> <ETX> <CR> <LF> \xHH 사용 가능 (공백 유지)
//   예) send 7 <STX>W1107C001X<ETX>
//   ota 7 build\agv_wifi_bridge.bin  → 이 PC 가 HTTP(8070) 로 파일을 서비스하고 ESP 에 URL 전달
//   cfg 7 set ip 192.168.10.107       → 원격 설정 (콘솔과 같은 키). cfg 7 = 현재 설정, cfg 7 save / cfg 7 reboot
//   dspota 7 stage AGV_Main_1.2.3.dsppkg apply
//                                     → 메인보드 펌웨어: 패키지(.dsppkg)만 받음 (맨 .dspfw 거부). 이 PC 가 HTTP 로 패키지와
//                                       그 안의 .dspfw 를 서비스하고 START 에 pkg_* 를 붙여 보냄 → 브리지가 패키지를 보관하고
//                                       .dspfw 를 받아 두고(스테이징) 바로 적용 (브리지 0.9.0+)
//   dspota 7 apply                    → 스테이징된 이미지 적용 (AGV 정지 상태에서). job 은 이 도구로 마지막 stage 한 번호를 씀 —
//                                       도구를 다시 띄웠으면 dspota 7 apply <job> (info·heartbeat 의 staged.job)
//   dspota 7 apply recover            → 구조 적용: 기동 불능 앱 복구 (브리지가 HELLO 를 50ms 간격으로 계속 보내는 동안
//                                       메인보드 전원을 다시 넣으면 부트로더의 부팅 직후 HELLO 창으로 진입)
//                                       앱이 프레임을 계속 보내는데 U 에 응답하지 않으면(구 앱) recover 여도 app_no_fwup 으로 끝남
//   info 7                            → 장치 정보 (ESP/메인보드 버전, 스테이징 상태)
using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Globalization;
using System.IO.Compression;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

const int MainPort = 50000;   // ESP server_port (MAIN_UP / STATUS / PONG 수신, 하향 송신)
const int RfidPort = 50001;   // ESP rfid_port (RFID 브로드캐스트 수신)
const int HttpPort = 8070;    // OTA 펌웨어 파일 서비스용

var agvs = new ConcurrentDictionary<byte, Agv>();
var logLock = new object();
var showHb = false;
// 테스트용 작업 번호 — 실행마다 다른 값에서 시작 (브리지 0.5.2 는 같은 번호의 START/APPLY 를 중복으로 보고 다시 받거나 굽지 않음)
// 게이트웨이와 같은 방식: (유닉스 초 % 10,000,000) × 100, u32 범위 안
var dspJob = (int)(DateTimeOffset.UtcNow.ToUnixTimeSeconds() % 10_000_000) * 100;
var stagedJob = new Dictionary<int, int>();   // 호기별 마지막 stage 의 job — apply 는 같은 job 으로 보내야 함 (다르면 job_mismatch). 한 번 적용하면 지움:
                                              // 같은 job 의 APPLY 는 브리지가 10분 동안 이전 결과만 다시 보내므로(다시 굽지 않음) 다시 하려면 stage 부터

using var mainSock = OpenUdp(MainPort);
using var rfidSock = OpenUdp(RfidPort);
using var cts = new CancellationTokenSource();
var rxMain = RxLoop(mainSock, cts.Token);
var rxRfid = RxLoop(rfidSock, cts.Token);

Log($"listening udp {MainPort} (main/status/pong), {RfidPort} (rfid)");
Log("commands: list | send <agv> <data> | ping <agv> [count] | ota <agv> <file.bin|url> | cfg <agv> [cmd] | info <agv> | dspota <agv> stage <file.dsppkg> [apply] | dspota <agv> apply [recover] [job] | hb | quit");

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
                FwServer.Serve("/fw.bin", File.ReadAllBytes(p[2]), HttpPort, Log);
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
            // dspota <agv> stage <file.dsppkg> [apply] | dspota <agv> apply [recover] [job]
            var sub = p[2].Split(' ', 3, StringSplitOptions.RemoveEmptyEntries);
            string json;
            if (sub[0] == "apply")
            {
                // 적용은 스테이징 때와 같은 job — 마지막 stage 의 job, 또는 마지막 인자로 준 번호(이 도구를 다시 띄운 경우, heartbeat staged.job)
                var recover = sub.Skip(1).Contains("recover");
                int job;
                if (sub.Length >= 2 && int.TryParse(sub[^1], out var given)) job = given;
                else if (!stagedJob.Remove(ag.Id, out job)) { Log($"[{ag.Id:D2}] 먼저 'dspota {ag.Id} stage …' (이미 적용한 job 을 다시 보내면 브리지는 이전 결과만 다시 보냄) — 또는 'dspota {ag.Id} apply [recover] <job>' (heartbeat staged.job)"); break; }
                json = recover ? $"{{\"job\":{job},\"recover\":1}}" : $"{{\"job\":{job}}}";
                if (recover) Log($"[{ag.Id:D2}] 구조 적용: 브리지가 HELLO 를 반복 송신하는 동안 메인보드 전원을 껐다 켜세요");
                var pkt = Proto.Build(Proto.DspOtaApply, ag.Id, ag.NextDownSeq(), Encoding.ASCII.GetBytes(json));
                mainSock.Send(pkt, pkt.Length, ag.Ep);
                Log($"[{ag.Id:D2}] DSP_OTA_APPLY {json}");
            }
            else if (sub[0] == "stage" && sub.Length >= 2)
            {
                var apply = sub.Length == 3 && sub[2] == "apply" ? 1 : 0;
                if (sub[1].StartsWith("http", StringComparison.OrdinalIgnoreCase))
                {
                    Log("URL 은 받지 않음 — 패키지(.dsppkg) 파일 경로를 주세요 (브리지 0.9.0 은 패키지와 .dspfw 를 함께 받음)");
                    break;
                }
                if (!File.Exists(sub[1])) { Log($"file not found: {sub[1]}"); break; }
                if (!DspPkg.TryLoad(sub[1], out var pk, out var why)) { Log(why); break; }
                // 패키지와 그 안의 .dspfw 를 함께 서비스 (브리지가 패키지를 먼저 받아 보관한 뒤 .dspfw 를 스테이징)
                FwServer.Serve("/fw.dspfw", pk.Dspfw, HttpPort, Log);
                FwServer.Serve("/pkg.dsppkg", pk.Package, HttpPort, Log);
                var host = $"http://{LocalIpToward(ag.Ep!)}:{HttpPort}";
                var job = ++dspJob;
                if (apply == 0) stagedJob[ag.Id] = job; else stagedJob.Remove(ag.Id);   // stage … apply 는 받자마자 굽는다
                json = $"{{\"url\":\"{host}/fw.dspfw\",\"ver\":\"{pk.Ver}\",\"job\":{job},\"apply\":{apply},\"crc32\":{pk.FileCrc},\"size\":{pk.Dspfw.Length}," +
                       $"\"pkg_url\":\"{host}/pkg.dsppkg\",\"pkg_size\":{pk.Package.Length},\"pkg_sha256\":\"{pk.Sha256}\",\"pkg_id\":\"{pk.Id}\"}}";
                Log($"[{ag.Id:D2}] package {Path.GetFileName(sub[1])}: ver {pk.Ver}, id {pk.Id}, {pk.Package.Length} B, .dspfw {pk.DspfwName} {pk.Dspfw.Length} B");
                var pkt = Proto.Build(Proto.DspOtaStart, ag.Id, ag.NextDownSeq(), Encoding.ASCII.GetBytes(json));
                mainSock.Send(pkt, pkt.Length, ag.Ep);
                Log($"[{ag.Id:D2}] DSP_OTA_START {json}");
            }
            else
            {
                Log("usage: dspota <agv> stage <file.dsppkg> [apply] | dspota <agv> apply [recover] [job]");
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

// ───────── 메인보드 패키지 (.dsppkg): 브리지는 풀지 않으므로 여기서 .dspfw 만 꺼내고 ID·해시를 계산 ─────────
sealed record DspPkg(byte[] Package, byte[] Dspfw, string DspfwName, string Ver, uint FileCrc, string Sha256, string Id)
{
    // 패키지 검증은 간단히 (전체 검증은 dsp_pack.py verify / 게이트웨이). 맨 .dspfw 는 거부
    public static bool TryLoad(string path, out DspPkg pkg, out string why)
    {
        pkg = null!;
        var data = File.ReadAllBytes(path);
        if (data.Length >= 7 && Encoding.ASCII.GetString(data, 0, 7) == "AGVDSP1")
        {
            why = "맨 .dspfw 는 보낼 수 없음 — .out/.map 을 함께 묶은 패키지(.dsppkg)만 받습니다 (dsp_pack.py bundle app.out --map app.map --ver <버전>)";
            return false;
        }
        if (data.Length > 6_287_360) { why = $"패키지({data.Length} B)가 브리지 보관 한도(6,287,360 B)를 넘음"; return false; }
        try
        {
            using var zip = new ZipArchive(new MemoryStream(data), ZipArchiveMode.Read);
            var mfEntry = zip.GetEntry("manifest.json") ?? throw new InvalidDataException("manifest.json 없음");
            byte[] mf;
            using (var ms = new MemoryStream())
            {
                using (var s = mfEntry.Open()) s.CopyTo(ms);
                mf = ms.ToArray();
            }
            using var doc = JsonDocument.Parse(mf);
            var root = doc.RootElement;
            if (root.GetProperty("format").GetString() != "agv-dsppkg" || root.GetProperty("format_ver").GetInt32() != 1)
                throw new InvalidDataException("format 이 agv-dsppkg v1 이 아님");
            var ver = root.GetProperty("ver").GetString() ?? "";
            var name = root.GetProperty("files").GetProperty("dspfw").GetProperty("name").GetString() ?? "";
            if (name.Contains('/') || name.Contains('\\') || !name.EndsWith(".dspfw", StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException($"manifest 의 .dspfw 이름이 이상함: {name}");
            var e = zip.GetEntry(name) ?? throw new InvalidDataException($"manifest 의 .dspfw({name}) 가 ZIP 에 없음");
            if (e.Length > 393_152) throw new InvalidDataException(".dspfw 가 393,152 B 를 넘음");
            byte[] fw;
            using (var ms = new MemoryStream())
            {
                using (var s = e.Open()) s.CopyTo(ms);
                fw = ms.ToArray();
            }
            if (fw.Length < 32 || Encoding.ASCII.GetString(fw, 0, 7) != "AGVDSP1") throw new InvalidDataException($"{name} 이 .dspfw 가 아님");
            var fver = Encoding.ASCII.GetString(fw, 16, 16).TrimEnd('\0');
            if (fver != ver) throw new InvalidDataException($"manifest ver({ver}) 과 .dspfw 버전({fver})이 다름");
            pkg = new DspPkg(data, fw, name, ver, BinaryPrimitives.ReadUInt32LittleEndian(fw.AsSpan(12)),
                             Convert.ToHexStringLower(SHA256.HashData(data)), Convert.ToHexStringLower(SHA256.HashData(mf))[..16]);
            why = "";
            return true;
        }
        catch (Exception ex) when (ex is InvalidDataException or KeyNotFoundException or JsonException or InvalidOperationException)
        {
            why = $"패키지(.dsppkg)가 아니거나 형식 오류: {ex.Message}";
            return false;
        }
    }
}

// ───────── OTA 파일 서비스: 최소 HTTP 서버 (GET 전용, 경로별 파일 — 모르는 경로는 마지막 Serve 파일) ─────────
static class FwServer
{
    static TcpListener? _l;
    static readonly ConcurrentDictionary<string, byte[]> _files = new();
    static byte[] _last = [];

    public static void Serve(string path, byte[] data, int port, Action<string> log)
    {
        _files[path] = data;
        _last = data;
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
            var line = req.ToString().Split("\r\n")[0];
            var parts = line.Split(' ');
            var path = parts.Length >= 2 ? parts[1].Split('?')[0] : "/";
            var data = _files.TryGetValue(path, out var f) ? f : _last;
            log($"http {c.Client.RemoteEndPoint} {line} -> {data.Length} bytes");
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
