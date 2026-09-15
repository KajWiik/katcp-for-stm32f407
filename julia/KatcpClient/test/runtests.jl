using Test
using KatcpClient
using Sockets

@testset "escape_arg / unescape_arg round-trip" begin
    for s in ["hello world", "", "a\\b", "line\nbreak", "tab\ttab",
              "esc\eape", "nul\0byte", "plain", "multi  space  run"]
        @test unescape_arg(escape_arg(s)) == s
    end
end

@testset "escape_arg exact output (matches katcp_proto.c's emit_escaped_arg)" begin
    @test escape_arg("hello world") == "hello\\_world"
    @test escape_arg("") == "\\@"
    @test escape_arg("a\\b") == "a\\\\b"
    @test escape_arg("line\nbreak") == "line\\nbreak"
    @test escape_arg("tab\ttab") == "tab\\ttab"
    @test escape_arg("esc\eape") == "esc\\eape"
    @test escape_arg("nul\0byte") == "nul\\0byte"
    @test escape_arg("plain") == "plain"
end

@testset "tokenize_line" begin
    @test tokenize_line("watchdog") == ("watchdog", nothing, String[])
    @test tokenize_line("synth-frequency 1000000000") ==
          ("synth-frequency", nothing, ["1000000000"])
    @test tokenize_line("halt[7]") == ("halt", "7", String[])
    @test tokenize_line("help synth-frequency") ==
          ("help", nothing, ["synth-frequency"])
    @test tokenize_line("synth-write-register[3] 0x700000") ==
          ("synth-write-register", "3", ["0x700000"])
    # escaped space inside an argument must not split it
    @test tokenize_line("cmd a\\_b c") == ("cmd", nothing, ["a b", "c"])
end

@testset "parse_line" begin
    r = parse_line("!watchdog ok")
    @test r isa KatcpReply
    @test r.name == "watchdog" && r.mid === nothing && r.status == "ok" && r.args == String[]

    r2 = parse_line("!synth-write-register[3] fail synth\\_register\\_write\\_failed")
    @test r2 isa KatcpReply
    @test r2.mid == "3"
    @test r2.status == "fail"
    @test r2.args == ["synth register write failed"]

    i = parse_line("#help halt Halt\\_the\\_device.")
    @test i isa KatcpInform
    @test i.name == "help"
    @test i.args == ["halt", "Halt the device."]

    @test parse_line("") === nothing
end

@testset "check_ok" begin
    ok_reply = KatcpReply("watchdog", nothing, "ok", String[])
    @test check_ok(ok_reply) === ok_reply

    fail_reply = KatcpReply("synth-write-register", "1", "fail", ["nope"])
    @test_throws KatcpRequestFailed check_ok(fail_reply)
end

# =============================================================================
# Live loopback test: a minimal fake KATCP-speaking TCP server, run in a
# background task, talked to over a real socket by the real client code
# above -- not just string-level unit tests. Exercises connect_katcp,
# request, and request_pipeline (including out-of-order replies) against
# actual socket I/O.
# =============================================================================

function fake_katcp_server(port::Integer, script::Function)
    server = listen(port)
    task = @async begin
        sock = accept(server)
        try
            script(sock)
        finally
            close(sock)
        end
    end
    return server, task
end

@testset "live loopback: request()" begin
    port = 27147
    server, task = fake_katcp_server(port, sock -> begin
        line = readline(sock)
        @assert line == "?watchdog[1]"
        write(sock, "!watchdog[1] ok\n")
    end)

    conn = connect_katcp("127.0.0.1", port)
    reply, informs = request(conn, "watchdog")
    @test reply.status == "ok"
    @test isempty(informs)

    disconnect!(conn)
    close(server)
    wait(task)
end

@testset "live loopback: request() collects informs before the reply" begin
    port = 27148
    server, task = fake_katcp_server(port, sock -> begin
        line = readline(sock)
        @assert line == "?help[1]"
        write(sock, "#help halt Halt\\_the\\_device.\n")
        write(sock, "#help watchdog Check\\_the\\_server\\_is\\_alive.\n")
        write(sock, "!help[1] ok 2\n")
    end)

    conn = connect_katcp("127.0.0.1", port)
    reply, informs = request(conn, "help")
    @test reply.status == "ok"
    @test reply.args == ["2"]
    @test length(informs) == 2
    @test informs[1].args[1] == "halt"
    @test informs[2].args[1] == "watchdog"

    disconnect!(conn)
    close(server)
    wait(task)
end

@testset "live loopback: request_pipeline() with deliberately out-of-order replies" begin
    port = 27149
    server, task = fake_katcp_server(port, sock -> begin
        # Read all 3 pipelined requests first (client sends them without
        # waiting), THEN reply out of order -- the point of this test is
        # that request_pipeline() must still return results in the
        # ORIGINAL arglist order, not server reply order.
        lines = [readline(sock) for _ in 1:3]
        @assert lines == [
            "?synth-write-register[1] 0x700000",
            "?synth-write-register[2] 0x090021",
            "?synth-write-register[3] 0x00251C",
        ]
        write(sock, "!synth-write-register[3] fail synth\\_register\\_write\\_failed\n")
        write(sock, "!synth-write-register[1] fail synth\\_register\\_write\\_failed\n")
        write(sock, "!synth-write-register[2] fail synth\\_register\\_write\\_failed\n")
    end)

    conn = connect_katcp("127.0.0.1", port)
    replies = request_pipeline(conn, "synth-write-register",
        [["0x700000"], ["0x090021"], ["0x00251C"]])

    @test length(replies) == 3
    @test replies[1].mid == "1"
    @test replies[2].mid == "2"
    @test replies[3].mid == "3"
    @test all(r -> r.status == "fail", replies)

    disconnect!(conn)
    close(server)
    wait(task)
end

@testset "live loopback: request() tolerates a server that doesn't echo the mid" begin
    # Regression test for a real-world finding: tcpborphserver3
    # (katcp-library v0.2.0) replies to "?watchdog[1]" with plain
    # "!watchdog ok" -- no mid at all. Also sends unsolicited
    # #version-connect informs immediately on connect, before any
    # reply -- mimicked here too, since that's what actually triggered
    # investigating this in the first place.
    port = 27151
    server, task = fake_katcp_server(port, sock -> begin
        write(sock, "#version-connect katcp-library v0.2.0-304-gccea3d3-dirty 2026-09-12T20:19:43\n")
        write(sock, "#version-connect katcp-protocol 5.0-M\n")
        line = readline(sock)
        @assert line == "?watchdog[1]"
        write(sock, "!watchdog ok\n")  # deliberately no [1]
    end)

    conn = connect_katcp("127.0.0.1", port)
    reply, informs = request(conn, "watchdog")
    @test reply.status == "ok"
    @test reply.mid === nothing
    @test length(informs) == 2
    @test informs[1].name == "version-connect"

    disconnect!(conn)
    close(server)
    wait(task)
end
