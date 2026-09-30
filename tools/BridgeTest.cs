// AGV Wi-Fi 브리지 벤치 테스트용 UDP 서버 (.NET 10 file-based app)
// 실행: dotnet run BridgeTest.cs
// 명령: list | send <agv> <data> | ping <agv> [횟수] | hb | quit
//   data 표기: <STX> <ETX> <CR> <LF> \xHH 사용 가능 (공백 유지)
//   예) send 7 <STX>W1107C001X<ETX>
using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Text;

const int MainPort = 50000;   // ESP server_port (MAIN_UP / STATUS / PONG 수신, 하향 송신)
const int RfidPort = 50001;   // ESP rfid_port (RFID 브로드캐스트 수신)

var agvs = new ConcurrentDictionary<byte, Agv>();
var logLock = new object();
var showHb = false;

using var mainSock = OpenUdp(MainPort);
using var rfidSock = OpenUdp(RfidPort);
using var cts = new CancellationTokenSource();
var rxMain = RxLoop(mainSock, cts.Token);
var rxRfid = RxLoop(rfidSock, cts.Token);

Log($"listening udp {MainPort} (main/status/pong), {RfidPort} (rfid)");
Log("commands: list | send <agv> <data> | ping <agv> [count] | hb | quit");

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

// ───────── 패킷 규격 (펌웨어 proto.h 와 동일) ─────────
static class Proto
{
    public const byte MainUp = 0x01, MainDown = 0x02, Rfid = 0x03, Status = 0x10, Ping = 0x20, Pong = 0x21;
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
