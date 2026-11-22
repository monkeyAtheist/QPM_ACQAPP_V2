#!/usr/bin/env python3
"""Generate tests/fixtures/scipy_reference.inc: SciPy results the C++ core is checked against.

Run from the repository root:  python tools/reference/make_scipy_reference.py
Needs numpy and scipy. The input signal is a deterministic LCG noise plus two tones, reproduced exactly
by tests/signals.h, so no random generator has to agree between Python and C++.
"""
import pathlib

import numpy as np
import scipy
from scipy import signal

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / "tests" / "fixtures" / "scipy_reference.inc"

FAMILY = {"butter": 0, "cheby1": 1, "cheby2": 2, "bessel": 3, "ellip": 4}
RESPONSE = {"lowpass": 0, "highpass": 1, "bandpass": 2, "bandstop": 3}
WINDOW = {"boxcar": 0, "hann": 1, "hamming": 2, "blackman": 3, "blackmanharris": 4, "flattop": 5, "kaiser": 6}


def lcg_noise(n, seed):
    out = np.empty(n)
    state = seed & 0xFFFFFFFF
    for i in range(n):
        state = (1664525 * state + 1013904223) & 0xFFFFFFFF
        out[i] = state / 4294967296.0 - 0.5
    return out


def test_signal(n, fs):
    t = np.arange(n) / fs
    return lcg_noise(n, 12345) + 0.7 * np.sin(2 * np.pi * 1000.0 * t) + 0.3 * np.sin(2 * np.pi * 37.0 * t + 0.4)


def fmt(values):
    return "{" + ", ".join("%.17g" % float(v) for v in values) + "}"


IIR_CASES = [
    # name, family, response, order, fs, f1, f2, rp, rs
    ("butter_lp4", "butter", "lowpass", 4, 10000.0, 1000.0, 0.0, 1.0, 40.0),
    ("butter_hp6", "butter", "highpass", 6, 10000.0, 1500.0, 0.0, 1.0, 40.0),
    ("butter_bp4", "butter", "bandpass", 4, 10000.0, 1000.0, 2000.0, 1.0, 40.0),
    ("butter_bs3", "butter", "bandstop", 3, 10000.0, 1000.0, 2000.0, 1.0, 40.0),
    ("cheby1_lp5", "cheby1", "lowpass", 5, 10000.0, 1000.0, 0.0, 1.0, 40.0),
    ("cheby1_lp4", "cheby1", "lowpass", 4, 10000.0, 1000.0, 0.0, 0.5, 40.0),
    ("cheby1_hp7", "cheby1", "highpass", 7, 48000.0, 3000.0, 0.0, 0.1, 40.0),
    ("cheby2_lp5", "cheby2", "lowpass", 5, 10000.0, 1000.0, 0.0, 1.0, 40.0),
    ("cheby2_hp4", "cheby2", "highpass", 4, 10000.0, 1000.0, 0.0, 1.0, 50.0),
    ("cheby2_bs5", "cheby2", "bandstop", 5, 48000.0, 900.0, 1100.0, 1.0, 60.0),
    ("bessel_lp4", "bessel", "lowpass", 4, 10000.0, 1000.0, 0.0, 1.0, 40.0),
    ("bessel_lp9", "bessel", "lowpass", 9, 48000.0, 2000.0, 0.0, 1.0, 40.0),
    ("bessel_bp3", "bessel", "bandpass", 3, 48000.0, 500.0, 2000.0, 1.0, 40.0),
    # Cases the old application got wrong (direct form b/a): audit of 20/11/2026.
    ("butter_lp8_100hz", "butter", "lowpass", 8, 48000.0, 100.0, 0.0, 1.0, 40.0),
    ("butter_lp10_20hz", "butter", "lowpass", 10, 48000.0, 20.0, 0.0, 1.0, 40.0),
    ("butter_bp6_50_70hz", "butter", "bandpass", 6, 48000.0, 50.0, 70.0, 1.0, 40.0),
    ("cheby1_bp6_300_400hz", "cheby1", "bandpass", 6, 48000.0, 300.0, 400.0, 1.0, 40.0),
    ("cheby1_lp10_500hz", "cheby1", "lowpass", 10, 48000.0, 500.0, 0.0, 1.0, 40.0),
    ("butter_lp16_1khz", "butter", "lowpass", 16, 48000.0, 1000.0, 0.0, 1.0, 40.0),
    # Elliptic (Cauer): ripple rp in the pass band, at least rs down in the stop band.
    ("ellip_lp1", "ellip", "lowpass", 1, 10000.0, 1000.0, 0.0, 1.0, 40.0),
    ("ellip_lp4", "ellip", "lowpass", 4, 10000.0, 1000.0, 0.0, 1.0, 40.0),
    ("ellip_lp5", "ellip", "lowpass", 5, 48000.0, 3000.0, 0.0, 0.5, 60.0),
    ("ellip_hp6", "ellip", "highpass", 6, 48000.0, 2000.0, 0.0, 0.1, 80.0),
    ("ellip_bp4", "ellip", "bandpass", 4, 10000.0, 1000.0, 2000.0, 1.0, 50.0),
    ("ellip_bs3", "ellip", "bandstop", 3, 48000.0, 900.0, 1100.0, 0.5, 60.0),
    ("ellip_lp8_100hz", "ellip", "lowpass", 8, 48000.0, 100.0, 0.0, 0.1, 100.0),
]

FIR_CASES = [
    # name, response, taps, fs, f1, f2, window, beta
    ("hamming_lp127", "lowpass", 127, 48000.0, 1000.0, 0.0, "hamming", 0.0),
    ("kaiser_bp101", "bandpass", 101, 10000.0, 1000.0, 2000.0, "kaiser", 8.6),
    ("hann_hp63", "highpass", 63, 10000.0, 2000.0, 0.0, "hann", 0.0),
    ("blackman_bs81", "bandstop", 81, 10000.0, 1500.0, 2500.0, "blackman", 0.0),
    ("boxcar_lp32", "lowpass", 32, 1000.0, 100.0, 0.0, "boxcar", 0.0),
]

REMEZ_CASES = [
    # name, taps, fs, band edges, desired per band, weight per band
    ("lp_odd_51", 51, 10000.0, [0, 1000, 1500, 5000], [1, 0], [1, 1]),
    ("lp_even_64", 64, 48000.0, [0, 4000, 5000, 24000], [1, 0], [1, 10]),
    ("hp_65", 65, 10000.0, [0, 1500, 2000, 5000], [0, 1], [10, 1]),
    ("bp_101", 101, 10000.0, [0, 800, 1000, 2000, 2200, 5000], [0, 1, 0], [10, 1, 10]),
    ("bs_81", 81, 10000.0, [0, 900, 1200, 1800, 2100, 5000], [1, 0, 1], [1, 20, 1]),
    ("lp_long_301", 301, 48000.0, [0, 1000, 1200, 24000], [1, 0], [1, 100]),
    # Far better than asked (about 160 dB): extremal frequencies packed together.
    ("lp_overdesigned_101", 101, 10000.0, [0, 1300, 2700, 5000], [1, 0], [1, 5.7728]),
]

ORDER_CASES = [
    # name, family, response, fs, pass band edges, stop band edges, gpass (Ap), gstop (As)
    ("butter_lp", "butter", "lowpass", 48000.0, [1000.0], [2000.0], 1.0, 40.0),
    ("butter_hp", "butter", "highpass", 10000.0, [1500.0], [1000.0], 0.5, 50.0),
    ("butter_bp", "butter", "bandpass", 48000.0, [1000.0, 2000.0], [500.0, 4000.0], 1.0, 40.0),
    ("butter_bs_mains", "butter", "bandstop", 48000.0, [45.0, 55.0], [49.0, 51.0], 1.0, 30.0),
    ("butter_lp_1mhz", "butter", "lowpass", 1e6, [20000.0], [30000.0], 3.0, 60.0),
    ("cheby1_lp", "cheby1", "lowpass", 48000.0, [1000.0], [1500.0], 0.5, 60.0),
    ("cheby1_hp", "cheby1", "highpass", 44100.0, [300.0], [200.0], 0.1, 40.0),
    ("cheby1_bp_phone", "cheby1", "bandpass", 8000.0, [300.0, 3400.0], [200.0, 3600.0], 0.5, 40.0),
    ("cheby1_bs", "cheby1", "bandstop", 10000.0, [1000.0, 2500.0], [1500.0, 2000.0], 1.0, 50.0),
    ("cheby2_lp", "cheby2", "lowpass", 48000.0, [1000.0], [2000.0], 1.0, 40.0),
    ("cheby2_hp", "cheby2", "highpass", 48000.0, [100.0], [50.0], 0.5, 60.0),
    ("cheby2_bp", "cheby2", "bandpass", 48000.0, [1000.0, 3000.0], [600.0, 3600.0], 0.5, 50.0),
    ("cheby2_bs_mains", "cheby2", "bandstop", 48000.0, [45.0, 55.0], [49.0, 51.0], 1.0, 30.0),
    ("ellip_lp", "ellip", "lowpass", 48000.0, [1000.0], [1500.0], 0.5, 60.0),
    ("ellip_hp", "ellip", "highpass", 48000.0, [2000.0], [1800.0], 0.1, 80.0),
    ("ellip_bp", "ellip", "bandpass", 48000.0, [1000.0, 3000.0], [600.0, 3600.0], 0.5, 50.0),
    ("ellip_bs", "ellip", "bandstop", 10000.0, [1000.0, 2500.0], [1500.0, 2000.0], 1.0, 50.0),
    ("ellip_lp_narrow", "ellip", "lowpass", 48000.0, [1000.0], [1100.0], 0.1, 80.0),
]

WELCH_CASES = [
    # name, window, nperseg, noverlap
    ("hann_256_128", "hann", 256, 128),
    ("blackmanharris_512_384", "blackmanharris", 512, 384),
    ("flattop_1000_500", "flattop", 1000, 500),
]


def main():
    fs_signal = 48000.0
    n = 3000
    stride = 3  # every third output sample is stored
    x = test_signal(n, fs_signal)
    lines = [
        "// Generated by tools/reference/make_scipy_reference.py (numpy %s, scipy %s). Do not edit."
        % (np.__version__, scipy.__version__),
        "// Input: tests/signals.h testSignal(3000, 48000); filtered outputs keep every 3rd sample.",
        "",
        "inline const std::vector<IirReference> kIirReferences = {",
    ]
    for name, fam, resp, order, fs, f1, f2, rp, rs in IIR_CASES:
        wn = f1 if resp in ("lowpass", "highpass") else [f1, f2]
        if fam == "butter":
            sos = signal.butter(order, wn, btype=resp, fs=fs, output="sos")
        elif fam == "cheby1":
            sos = signal.cheby1(order, rp, wn, btype=resp, fs=fs, output="sos")
        elif fam == "cheby2":
            sos = signal.cheby2(order, rs, wn, btype=resp, fs=fs, output="sos")
        elif fam == "ellip":
            sos = signal.ellip(order, rp, rs, wn, btype=resp, fs=fs, output="sos")
        else:
            sos = signal.bessel(order, wn, btype=resp, fs=fs, output="sos", norm="mag")
        freqs = np.geomspace(fs * 1e-4, fs * 0.49, 48)
        _, h = signal.sosfreqz(sos, worN=freqs, fs=fs)
        mag = 20 * np.log10(np.maximum(np.abs(h), 1e-300))
        # Same rate for the input: the filter's own fs (the sample values do not care).
        y = signal.sosfilt(sos, x)
        yy = signal.sosfiltfilt(sos, x)
        lines.append(
            '    {"%s", %d, %d, %d, %r, %r, %r, %r, %r, %s, %s, %s, %s},'
            % (name, FAMILY[fam], RESPONSE[resp], order, fs, f1, f2, rp, rs, fmt(freqs), fmt(mag), fmt(y[::stride]), fmt(yy[::stride]))
        )
    lines.append("};")
    lines.append("")
    lines.append("inline const std::vector<FirReference> kFirReferences = {")
    for name, resp, taps, fs, f1, f2, win, beta in FIR_CASES:
        cutoff = f1 if resp in ("lowpass", "highpass") else [f1, f2]
        window = ("kaiser", beta) if win == "kaiser" else win
        h = signal.firwin(taps, cutoff, window=window, pass_zero=resp, fs=fs, scale=True)
        lines.append(
            '    {"%s", %d, %d, %r, %r, %r, %d, %r, %s},'
            % (name, RESPONSE[resp], taps, fs, f1, f2, WINDOW[win], beta, fmt(h))
        )
    lines.append("};")
    lines.append("")
    lines.append("inline const std::vector<RemezReference> kRemezReferences = {")
    for name, taps, fs, bands, desired, weights in REMEZ_CASES:
        h = signal.remez(taps, bands, desired, weight=weights, fs=fs)
        lines.append(
            '    {"%s", %d, %r, %s, %s, %s, %s},'
            % (name, taps, fs, fmt(bands), fmt(desired), fmt(weights), fmt(h))
        )
    lines.append("};")
    lines.append("")
    lines.append("inline const std::vector<OrderReference> kOrderReferences = {")
    order_functions = {"butter": signal.buttord, "cheby1": signal.cheb1ord, "cheby2": signal.cheb2ord, "ellip": signal.ellipord}
    for name, fam, resp, fs, passband, stopband, gpass, gstop in ORDER_CASES:
        wp = passband[0] if len(passband) == 1 else passband
        ws = stopband[0] if len(stopband) == 1 else stopband
        order, wn = order_functions[fam](wp, ws, gpass, gstop, fs=fs)
        lines.append('    {"%s", %d, %d, %r, %s, %s, %r, %r, %d, %s},'
                     % (name, FAMILY[fam], RESPONSE[resp], fs, fmt(passband), fmt(stopband), gpass, gstop, order,
                        fmt(np.atleast_1d(wn))))
    lines.append("};")
    lines.append("")
    lines.append("inline const std::vector<WelchReference> kWelchReferences = {")
    for name, win, nperseg, noverlap in WELCH_CASES:
        f, p = signal.welch(x, fs=fs_signal, window=win, nperseg=nperseg, noverlap=noverlap, detrend="constant",
                            scaling="density")
        lines.append('    {"%s", %d, %d, %d, %s},' % (name, WINDOW[win], nperseg, noverlap, fmt(p)))
    lines.append("};")
    lines.append("")
    lines.append("inline const std::vector<FiltFiltReference> kFiltFiltReferences = {")
    fs_ff = 48000.0
    t = np.arange(400) / fs_ff
    am = (1 + 0.5 * np.sin(2 * np.pi * 500 * t)) * np.cos(2 * np.pi * 10000 * t)  # starts on a crest
    ramp = 5 * t / t[-1] + np.sin(2 * np.pi * 200 * t)
    cases = [
        ("am_bandpass10", signal.butter(10, [10500, 15000], btype="bandpass", fs=fs_ff, output="sos"), None, am),
        ("ramp_lowpass4", signal.butter(4, 1000, fs=fs_ff, output="sos"), None, ramp),
        ("am_fir31", None, signal.firwin(31, 6000, fs=fs_ff), am),
    ]
    for name, sos, taps, xs in cases:
        for padtype in ("odd", "even", "constant", None):
            if sos is not None:
                y = signal.sosfiltfilt(sos, xs, padtype=padtype)
                coeffs, fir = sos.ravel(), []
            else:
                y = signal.filtfilt(taps, [1.0], xs, padtype=padtype)
                coeffs, fir = [], taps
            lines.append('    {"%s", "%s", %s, %s, %s, %s},'
                         % (name, padtype or "none", fmt(coeffs), fmt(fir), fmt(xs), fmt(y)))
    lines.append("};")
    lines.append("")
    lines.append("inline const std::vector<BesselPoleReference> kBesselPoles = {")
    for order in (1, 2, 3, 5, 8, 10, 12):
        _, p, k = signal.besselap(order, norm="mag")
        p = sorted(p, key=lambda c: (round(c.imag, 9), c.real))
        lines.append("    {%d, %s, %s}," % (order, fmt([c.real for c in p]), fmt([c.imag for c in p])))
    lines.append("};")
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("wrote", OUT, OUT.stat().st_size, "bytes")


if __name__ == "__main__":
    main()
