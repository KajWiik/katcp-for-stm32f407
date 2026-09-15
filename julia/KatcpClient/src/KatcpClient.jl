module KatcpClient

using Sockets

export KatcpConnection, connect_katcp, disconnect!,
       KatcpReply, KatcpInform, KatcpRequestFailed,
       request, request_pipeline, check_ok,
       escape_arg, unescape_arg, tokenize_line, parse_line

# =============================================================================
# Escaping -- mirrors common/src/katcp_proto.c exactly (see that file's
# escape table and IMPLEMENTATION.md §3.2). Every mapping here has a
# corresponding entry in the C server's unescape_arg()/emit_escaped_arg(),
# checked line-for-line against it, not derived independently.
# =============================================================================

const ESCAPE_OUT = Dict{Char,String}(
    ' '  => "\\_",
    '\n' => "\\n",
    '\r' => "\\r",
    '\t' => "\\t",
    '\e' => "\\e",
    '\0' => "\\0",
    '\\' => "\\\\",
)

"""
    escape_arg(s) -> String

Escapes `s` for use as one KATCP wire argument, per NRF-KAT7-6.0-IFCE-002.
An empty string becomes the `\\@` empty-argument marker.
"""
function escape_arg(s::AbstractString)::String
    isempty(s) && return "\\@"
    io = IOBuffer()
    for c in s
        write(io, get(ESCAPE_OUT, c, string(c)))
    end
    return String(take!(io))
end

"""
    unescape_arg(s) -> String

Inverse of [`escape_arg`](@ref). An unrecognized escape (`\\` followed
by any character not in the table) passes the following character
through literally, matching the server's behavior.
"""
function unescape_arg(s::AbstractString)::String
    io = IOBuffer()
    chars = collect(s)
    n = length(chars)
    i = 1
    while i <= n
        c = chars[i]
        if c == '\\' && i < n
            e = chars[i+1]
            if e == '_'
                write(io, ' ')
            elseif e == 'n'
                write(io, '\n')
            elseif e == 'r'
                write(io, '\r')
            elseif e == 't'
                write(io, '\t')
            elseif e == 'e'
                write(io, '\e')
            elseif e == '0'
                write(io, '\0')
            elseif e == '@'
                # empty-argument marker: contributes nothing
            elseif e == '\\'
                write(io, '\\')
            else
                write(io, e)
            end
            i += 2
        else
            write(io, c)
            i += 1
        end
    end
    return String(take!(io))
end

# =============================================================================
# Tokenizing -- mirrors katcp_proto.c's tokenize(). Splits on unescaped
# spaces, then unescapes each token. Separate from escape_arg/unescape_arg
# above (those act on ONE already-delimited argument).
# =============================================================================

"""
    tokenize_line(body) -> (name, mid, args)

Splits `body` (a request/reply/inform line with its leading `?`/`!`/`#`
already stripped) into the bare command/reply/inform name, an optional
`[mid]` (as a `String`, or `nothing`), and the unescaped argument list.
"""
function tokenize_line(body::AbstractString)
    chars = collect(body)
    n = length(chars)
    tokens = String[]
    cur = Char[]
    i = 1
    while i <= n
        c = chars[i]
        if c == '\\' && i < n
            push!(cur, c)
            push!(cur, chars[i+1])
            i += 2
            continue
        end
        if c == ' '
            if !isempty(cur)
                push!(tokens, String(cur))
                cur = Char[]
            end
            i += 1
            continue
        end
        push!(cur, c)
        i += 1
    end
    !isempty(cur) && push!(tokens, String(cur))

    tokens = unescape_arg.(tokens)
    isempty(tokens) && return (nothing, nothing, String[])

    name = tokens[1]
    mid = nothing
    lb = findfirst('[', name)
    if lb !== nothing && endswith(name, ']')
        mid = name[lb+1:end-1]
        name = name[1:lb-1]
    end
    return (name, mid, tokens[2:end])
end

# =============================================================================
# Message types
# =============================================================================

abstract type KatcpMessage end

"""A parsed `!name[mid] status arg...` reply line."""
struct KatcpReply <: KatcpMessage
    name::String
    mid::Union{String,Nothing}
    status::String
    args::Vector{String}
end

"""A parsed `#name[mid] arg...` inform line."""
struct KatcpInform <: KatcpMessage
    name::String
    mid::Union{String,Nothing}
    args::Vector{String}
end

"""
    parse_line(line) -> KatcpReply | KatcpInform | nothing

Parses one complete wire line (newline already stripped). Returns
`nothing` for a blank line (harmless, can occur from a lone `\\r`
terminator, mirroring the server's tolerance of that).
"""
function parse_line(line::AbstractString)::Union{KatcpReply,KatcpInform,Nothing}
    isempty(line) && return nothing
    prefix = line[1]
    body = line[2:end]
    name, mid, args = tokenize_line(body)
    name === nothing && return nothing
    if prefix == '!'
        isempty(args) && error("malformed reply line (no status word): $(repr(line))")
        return KatcpReply(name, mid, args[1], args[2:end])
    elseif prefix == '#'
        return KatcpInform(name, mid, args)
    else
        error("unexpected line prefix $(repr(prefix)) in $(repr(line)) -- " *
              "expected a reply ('!') or inform ('#') from the server")
    end
end

function format_request(name::AbstractString, args, mid::Union{Integer,Nothing})::String
    head = mid === nothing ? "?$name" : "?$name[$mid]"
    parts = String[head]
    for a in args
        push!(parts, escape_arg(string(a)))
    end
    return join(parts, ' ') * "\n"
end

# =============================================================================
# Connection
# =============================================================================

mutable struct KatcpConnection
    sock::TCPSocket
    next_mid::Int
end

"""
    connect_katcp(host, port=7147) -> KatcpConnection
"""
function connect_katcp(host::AbstractString, port::Integer=7147)
    sock = Sockets.connect(host, port)
    return KatcpConnection(sock, 1)
end

disconnect!(conn::KatcpConnection) = close(conn.sock)

function read_message(conn::KatcpConnection)
    while true
        line = readline(conn.sock)
        msg = parse_line(line)
        msg === nothing || return msg
    end
end

"""
    KatcpRequestFailed(reply)

Thrown by [`check_ok`](@ref) when a reply's status is not `"ok"`.
"""
struct KatcpRequestFailed <: Exception
    reply::KatcpReply
end

function Base.showerror(io::IO, e::KatcpRequestFailed)
    print(io, "KATCP request failed: !", e.reply.name, " ", e.reply.status,
          " ", join(e.reply.args, ' '))
end

"""
    check_ok(reply) -> reply

Returns `reply` unchanged if `reply.status == "ok"`, otherwise throws
[`KatcpRequestFailed`](@ref). `request`/`request_pipeline` deliberately
do NOT call this automatically -- inspecting `.status` yourself is
just as valid, this is a convenience for call sites that want
exception-based control flow instead.
"""
check_ok(reply::KatcpReply) = reply.status == "ok" ? reply :
    throw(KatcpRequestFailed(reply))

"""
    request(conn, name, args...) -> (reply, informs)

Sends one request (auto-tagged with the connection's next message ID),
blocks until its matching reply arrives, and returns it along with any
informs collected while waiting. Assumes no OTHER request is
concurrently in flight on this connection -- see
[`request_pipeline`](@ref) for sending several at once.

**Matching is deliberately tolerant of servers that don't echo the
mid back.** The KATCP spec allows message IDs, but not every real
server implements them on every reply -- confirmed directly against a
real `tcpborphserver3` (`katcp-library v0.2.0`), whose `!watchdog ok`
reply carried no `[mid]` at all in response to a `?watchdog[1]`
request. A reply counts as "the" reply to this request if either (a)
it carries a mid and that mid matches, or (b) it carries no mid at
all and its name matches -- but NOT if it carries some OTHER mid,
which would mean a genuinely different request's reply arrived here,
a real misuse this function still won't silently accept.
"""
function request(conn::KatcpConnection, name::AbstractString, args...)
    mid = conn.next_mid
    conn.next_mid += 1
    write(conn.sock, format_request(name, args, mid))
    informs = KatcpInform[]
    while true
        msg = read_message(conn)
        if msg isa KatcpInform
            push!(informs, msg)
        elseif msg isa KatcpReply && msg.name == name &&
               (msg.mid == string(mid) || msg.mid === nothing)
            return msg, informs
        end
        # A reply here with a DIFFERENT (non-nothing) mid, or a
        # different name entirely, would mean this connection has
        # another request in flight concurrently with this call, which
        # this function doesn't support -- request_pipeline() is the
        # one to use for that. Such a reply is silently skipped rather
        # than treated as an inform, since it isn't one.
    end
end

"""
    request_pipeline(conn, name, arglists) -> Vector{KatcpReply}

Sends one request per entry of `arglists` back-to-back, without
waiting for each individual reply, then reads replies until one has
arrived for every request -- matched back up by message ID, not
network arrival order, so the returned vector is in the SAME ORDER as
`arglists` regardless of how the server actually replied.

This is the way to send many requests (e.g. the ~113 individual
`?synth-write-register` calls needed to replay an LMX2594 register
dump) without paying one network round-trip per request: the server's
tokenizer already processes every complete line buffered in one
`recv()` and replies to each independently (see IMPLEMENTATION.md
§3.3), so pipelining like this is safe by construction, not a hack
that happens to work.

**Requires the server to echo mids back correctly on every reply to
work at all** -- unlike [`request`](@ref), there's no tolerant
fallback here, because with genuinely concurrent requests in flight
there's no way to know which un-tagged reply belongs to which request.
Confirmed to work against this project's own STM32-E407 firmware
(which does echo mids correctly). Confirmed, separately, that at least
one real-world server (`tcpborphserver3`/`katcp-library v0.2.0`) does
NOT echo mids even on a simple single-request reply -- pipelining
against a server like that would not work, and this function has no
way to detect or gracefully degrade from that; it would simply hang
the same way `request()` used to before its tolerant-matching fix.

Informs received along the way are currently discarded (not needed
for the register-write use case this was built for) -- extend this if
a future caller needs them per-reply.
"""
function request_pipeline(conn::KatcpConnection, name::AbstractString, arglists)
    n = length(arglists)
    mids = Vector{Int}(undef, n)
    for (i, args) in enumerate(arglists)
        mid = conn.next_mid
        conn.next_mid += 1
        mids[i] = mid
        write(conn.sock, format_request(name, args, mid))
    end
    flush(conn.sock)

    pending = Dict(string(m) => i for (i, m) in enumerate(mids))
    replies = Vector{Union{KatcpReply,Nothing}}(nothing, n)
    while !isempty(pending)
        msg = read_message(conn)
        msg isa KatcpReply || continue
        idx = get(pending, msg.mid, nothing)
        idx === nothing && continue  # reply for someone else's mid -- ignore
        replies[idx] = msg
        delete!(pending, msg.mid)
    end
    return Vector{KatcpReply}(replies)
end

end # module
