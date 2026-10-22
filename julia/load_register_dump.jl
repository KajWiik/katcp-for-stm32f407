#!/usr/bin/env julia
#
# Loads an LMX2594-style register dump (e.g. exported from PLLatinumSim)
# and replays it via ?synth-write-register, one request per register,
# pipelined for speed -- NOT a single bulk transfer. See the discussion
# in IMPLEMENTATION.md on why: KATCP's own per-request limits
# (KATCP_MAX_ARGS=16, KATCP_MAX_LINE=256 bytes) make a single
# ?synth-program-all-113-registers request a non-starter, and KATCP is
# fundamentally a line-oriented request/reply protocol, not a bulk
# transport -- fighting that with bigger buffers would be the wrong fix.
#
# Usage: julia load_register_dump.jl <host> <synth> <hexfile> [port]
#   <synth> is 1 or 2 -- which of the board's two LMX2594s (separate CS
#   lines) to program.

include(joinpath(@__DIR__, "KatcpClient", "src", "KatcpClient.jl"))
using .KatcpClient

"""
    parse_register_dump(path) -> Vector{UInt32}

Parses a register dump file with lines like:

    R112	0x700000
    R111	0x6F0000
    ...
    R0	0x00251C

-- one 24-bit register word per line, in the SAME ORDER as the file.
Order matters: LMX2594-style parts require register 0 (which triggers
VCO calibration) to be written LAST, and register-dump export tools
already order their output that way -- this function preserves that
order rather than re-sorting by register number, and `program_registers`
below relies on that (see its docstring).

Cross-checks each line's `R<n>` label against the register address
actually encoded in the value (bits [23:16]) and errors out on a
mismatch -- catching a corrupted or hand-edited dump file before it
gets anywhere near hardware, rather than silently writing the wrong
register.
"""
function parse_register_dump(path::AbstractString)::Vector{UInt32}
    words = UInt32[]
    for (lineno, raw) in enumerate(eachline(path))
        line = strip(raw)
        isempty(line) && continue

        parts = split(line)
        length(parts) == 2 ||
            error("$path:$lineno: expected 2 whitespace-separated fields, " *
                  "got $(length(parts)): $(repr(line))")

        label, valuestr = parts
        m = match(r"^R(\d+)$"i, label)
        m === nothing &&
            error("$path:$lineno: expected a register label like 'R112', " *
                  "got $(repr(label))")
        labeled_addr = parse(Int, m.captures[1])

        value = parse(UInt32, valuestr)  # parse() auto-detects the "0x" prefix
        value <= 0xFFFFFF ||
            error("$path:$lineno: value $(repr(valuestr)) doesn't fit in 24 bits")

        encoded_addr = Int((value >> 16) & 0x7F)
        encoded_addr == labeled_addr ||
            error("$path:$lineno: label says R$labeled_addr but the value's " *
                  "address field is R$encoded_addr -- refusing to guess which " *
                  "one is right")

        push!(words, value)
    end
    return words
end

"""
    program_registers(host, synth, words; port=7147) -> Vector{Bool}

Connects to `host`, replays `words` (in the order given -- see
[`parse_register_dump`](@ref) on why that order matters) to synth
number `synth` (1-based, as the firmware's KATCP commands count them)
via `?synth-write-register <synth> <word>`, pipelined over one
connection, and returns a
`Bool` per register indicating whether that individual write
succeeded (`status == "ok"`).

Does NOT throw on a per-register failure. With
`synth_write_raw_register()` currently stubbed to always report
failure server-side (`common/src/synth_stub.c` -- no real synth driver
exists yet), every entry will be `false`; that's expected, not a bug
in this client. Once a real driver exists, a genuine mid-sequence
failure is also something a caller may want to inspect rather than
have hidden behind an exception (e.g. to know exactly which register
didn't take before deciding whether to retry or abort).
"""
function program_registers(host::AbstractString, synth::Integer,
                            words::Vector{UInt32}; port::Integer=7147)
    conn = connect_katcp(host, port)
    try
        arglists = [[string(synth), "0x" * string(w, base=16, pad=6)] for w in words]
        replies = request_pipeline(conn, "synth-write-register", arglists)
        return [r.status == "ok" for r in replies]
    finally
        disconnect!(conn)
    end
end

function main()
    if length(ARGS) < 3
        println(stderr, "usage: julia load_register_dump.jl <host> <synth> <hexfile> [port]")
        exit(1)
    end
    host    = ARGS[1]
    synth   = parse(Int, ARGS[2])
    hexfile = ARGS[3]
    port    = length(ARGS) >= 4 ? parse(Int, ARGS[4]) : 7147

    words = parse_register_dump(hexfile)
    println("Parsed $(length(words)) register(s) from $hexfile")

    results = program_registers(host, synth, words; port=port)
    nfail = count(!, results)
    if nfail == 0
        println("All $(length(words)) register writes succeeded.")
    else
        println(stderr, "$nfail of $(length(words)) register write(s) FAILED:")
        for (i, (w, ok)) in enumerate(zip(words, results))
            ok || println(stderr, "  #$i: 0x$(string(w, base=16, pad=6)) -> fail")
        end
        exit(1)
    end
end

abspath(PROGRAM_FILE) == (@__FILE__) && main()
