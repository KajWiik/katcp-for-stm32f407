"""
lmx2594_loop_gain.jl

Julia port of lmx2594_loop_gain_fp.h, for debugging/verifying the CPG
selection logic before it goes into the ChibiOS firmware -- same
tables, same formulas, same rounding-to-nearest-available-current
behavior, just easy to sweep/plot/inspect interactively.

Reference: LMX2594 datasheet SNAS696C -- Table 6 (VCO Core Ranges),
Table 7 (VCO Gain) and its Equation 4, and Section 7.6.3 (CPG
register, R14[6:4]).
"""
module LMX2594LoopGain

export VcoBand, VCO_BANDS, CPG_LEVELS_MA, CPG_CODES,
       vco_core, kvco, icp_ideal_ma, nearest_cpg_code, select_cpg,
       FVCO_REF_MHZ, ICP_REF_MA

# ---- CONFIGURE THESE TWO ----
# The VCO frequency your physical loop filter (R2/C1/C2/C3/R3/C4) was
# actually designed/simulated for, and the charge-pump current used
# in that design. Everything else is normalized relative to this
# point.
const FVCO_REF_MHZ = 12000.0   # e.g. 12 GHz
const ICP_REF_MA   = 9.0       # e.g. 9 mA -> CPG code 5

struct VcoBand
    fmin_mhz::Float64
    fmax_mhz::Float64
    kvco1::Float64   # MHz/V, at fmin_mhz
    kvco2::Float64   # MHz/V, at fmax_mhz
end

# Datasheet Table 6 / Table 7, VCO1..VCO7 (the LMX2594 has 7 cores,
# not 8 -- VCO_SEL, R20[13:11], only accepts 1-7).
const VCO_BANDS = [
    VcoBand( 7500.0,  8600.0,  73.0, 114.0),  # VCO1
    VcoBand( 8600.0,  9800.0,  61.0, 121.0),  # VCO2
    VcoBand( 9800.0, 10800.0,  98.0, 132.0),  # VCO3
    VcoBand(10800.0, 12000.0, 106.0, 141.0),  # VCO4
    VcoBand(12000.0, 12900.0, 170.0, 215.0),  # VCO5
    VcoBand(12900.0, 13900.0, 172.0, 218.0),  # VCO6
    VcoBand(13900.0, 15000.0, 182.0, 239.0),  # VCO7
]

# Available charge-pump currents (mA) and their CPG register codes,
# ascending by current -- R14[6:4] per Section 7.6.3. States 2 and 6
# are reserved, so only these 6 currents actually exist.
const CPG_LEVELS_MA = [0.0, 3.0, 6.0, 9.0, 12.0, 15.0]
const CPG_CODES     = [0, 4, 1, 5, 3, 7]

"""
    vco_core(fvco_mhz) -> Int

Which of the 7 cores (1-indexed, matching VCO1..VCO7) covers
`fvco_mhz`. Returns 0 if out of the device's 7.5-15 GHz VCO range.
"""
function vco_core(fvco_mhz::Real)
    for (i, b) in enumerate(VCO_BANDS)
        if b.fmin_mhz <= fvco_mhz <= b.fmax_mhz
            return i
        end
    end
    return 0
end

"""
    kvco(fvco_mhz, core) -> Float64

Linear-interpolated Kvco (datasheet Equation 4), in MHz/V. `core`
must be a valid 1-indexed band from `vco_core`.
"""
function kvco(fvco_mhz::Real, core::Integer)
    b = VCO_BANDS[core]
    b.kvco1 + (b.kvco2 - b.kvco1) * (fvco_mhz - b.fmin_mhz) / (b.fmax_mhz - b.fmin_mhz)
end

"""
    icp_ideal_ma(fvco_mhz, kv) -> Float64

Ideal (continuous) charge-pump current in mA to hold the loop's
natural frequency constant relative to the reference design point,
assuming fixed fPD so N is proportional to fVCO:

    Icp_ideal = Icp_ref * (fVCO / fVCO_ref) * (Kvco_ref / Kvco)

If your fPD is NOT fixed across the tuning range, replace the
fvco_mhz/FVCO_REF_MHZ ratio below with N/N_ref (actual feedback
divider ratio) instead -- the rest of the math is unchanged.
"""
function icp_ideal_ma(fvco_mhz::Real, kv::Real)
    ref_core = vco_core(FVCO_REF_MHZ)
    kv_ref = kvco(FVCO_REF_MHZ, ref_core)
    ICP_REF_MA * (fvco_mhz / FVCO_REF_MHZ) * (kv_ref / kv)
end

"""
    nearest_cpg_code(icp_ideal) -> Int

Nearest available CPG register code for a desired current (mA).
"""
function nearest_cpg_code(icp_ideal::Real)
    idx = argmin(abs.(CPG_LEVELS_MA .- icp_ideal))
    CPG_CODES[idx]
end

"""
    select_cpg(fvco_mhz) -> (code, core, kv, icp_ideal, icp_actual)

Top-level entry point: given only the target VCO frequency (MHz,
before the channel divider), return the CPG register code to write
into R14[6:4], plus the intermediate values for debugging. `code` is
`-1` if `fvco_mhz` is outside 7.5-15 GHz.
"""
function select_cpg(fvco_mhz::Real)
    core = vco_core(fvco_mhz)
    core == 0 && return (-1, 0, NaN, NaN, NaN)

    kv = kvco(fvco_mhz, core)
    icp_ideal = icp_ideal_ma(fvco_mhz, kv)
    code = nearest_cpg_code(icp_ideal)
    icp_actual = CPG_LEVELS_MA[findfirst(==(code), CPG_CODES)]

    (code, core, kv, icp_ideal, icp_actual)
end

end # module

# ---------------------------------------------------------------
# Debug / verification driver
# ---------------------------------------------------------------
using .LMX2594LoopGain
using Printf

function print_sweep(step_mhz=100.0)
    println("fVCO(MHz)  core  Kvco(MHz/V)  Icp_ideal(mA)  CPG_code  Icp_actual(mA)  Icp*Kvco/fVCO")
    for f in 7500.0:step_mhz:15000.0
        code, core, kv, ideal, actual = select_cpg(f)
        ratio = actual * kv / f   # should be roughly flat if compensation is working
        @printf("%9.1f  %4d  %11.2f  %13.3f  %8d  %14.1f  %13.5f\n",
                f, core, kv, ideal, code, actual, ratio)
    end
end

print_sweep()

# Optional: visualize compensated vs. uncompensated loop gain proxy
# across the tuning range, if CairoMakie is available.
function plot_loop_gain_flatness(step_mhz=25.0)
    freqs = collect(7500.0:step_mhz:15000.0)
    fs = [f for f in freqs if vco_core(f) != 0]

    compensated  = Float64[]
    uncompensated = Float64[]
    for f in fs
        code, core, k, ideal, actual = select_cpg(f)
        push!(compensated, actual * k / f)          # normalized loop-gain proxy, compensated
        push!(uncompensated, ICP_REF_MA * k / f)     # same proxy, fixed reference Icp
    end

    fig = Figure(size = (900, 500))
    ax = Axis(fig[1, 1],
              xlabel = "fVCO (MHz)", ylabel = "Icp * Kvco / fVCO  (proxy for loop gain)",
              title = "CPG compensation across VCO cores")
    lines!(ax, fs, uncompensated, label = "fixed Icp = $(ICP_REF_MA) mA", linestyle = :dash)
    lines!(ax, fs, compensated, label = "compensated (nearest CPG code)")
    axislegend(ax, position = :rt)
    fig
end

# Uncomment if CairoMakie is loaded in your environment:
# using CairoMakie
# fig = plot_loop_gain_flatness()
# save("lmx2594_loop_gain_flatness.png", fig)
