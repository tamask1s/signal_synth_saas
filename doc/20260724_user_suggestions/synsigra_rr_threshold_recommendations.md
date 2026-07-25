# Synsigra — RR MAE és tol% küszöbértékek javaslat

Dátum: 2026-07-24  
Kontextus: r_peak_rr_snr_ladder_v1 v2.0, verifier v0.15.0, overlap-alapú RR párosítás  

## 3. Irodalmi alap

| Forrás | Mit mond az RR pontosságról |
|--------|-----------------------------|
| **IEC 60601-2-47** (Holter monitoring) | RR intervallum mérési hiba ≤ 10 ms (clean jel) |
| **ESC/NASPE HRV Task Force (1996)** | HRV használhatóságához ≥ 90-95% ectopic-mentes RR szükséges |
| **AAMI EC57 (2012)** | Beat detekciós tolerancia ±150 ms; RR-re közvetetten hat |
| **Satija et al. (2018)** | Klasszikus algoritmusok 0 dB-nél ~10-20 ms RR MAE; −5 dB-nél 40-80 ms |
| **Moody & Mark (2001)** | PhysioNet MIT-BIH: a matched RR-ek mediánja 2-5 ms clean jelen |
| **Fizikai korlát** | Egy FP kettévág egy ~800 ms RR-t → fragmentek hibája ~400 ms; ez az MAE-be beágyazódik |

---

## 4. Javasolt küszöbértékek

### RR tol% (tolerance pass fraction)

Tolerancia-definíció: ±10 ms abszolút VAGY ±2% relatív (amelyik nagyobb).

| SNR | Javasolt küszöb | Indoklás |
|-----|-----------------|----------|
| clean | ≥ 95% | IEC/HRV Task Force: ≥95% jó RR a HRV-hez |
| −0.2 dB | ≥ 80% | Kis zaj, 1-2 FP elviselhető |
| −0.5 dB | ≥ 75% | |
| −1 dB | ≥ 72% | |
| −2 dB | ≥ 55% | Több FP → több fragment → tol% esik |
| −3 dB | ≥ 45% | Az RR-ek ~fele még korrekt |
| −4 dB | ≥ 38% | |
| −5 dB | ≥ 35% | |
| −7 dB | ≥ 30% | |
| −9 dB | ≥ 25% | |
| −11 dB | ≥ 20% | Min. 1/5-nek jónak kell lennie |

### RR MAE (mean absolute error)

| SNR | Javasolt küszöb | Indoklás |
|-----|-----------------|----------|
| clean | ≤ 10 ms | IEC 60601-2-47 szint |
| −0.2 dB | ≤ 40 ms | 1-2 FP: max ~2×400ms/80 ≈ 10 ms hozzáadás |
| −0.5 dB | ≤ 50 ms | |
| −1 dB | ≤ 60 ms | |
| −2 dB | ≤ 70 ms | Irodalom: 0dB-nél 20 ms, −2dB-nél ~40-50 ms + fragment |
| −3 dB | ≤ 100 ms | |
| −4 dB | ≤ 140 ms | |
| −5 dB | ≤ 170 ms | Satija: −5dB-nél matched MAE ~60 ms + fragmentek |
| −7 dB | ≤ 200 ms | |
| −9 dB | ≤ 230 ms | |
| −11 dB | ≤ 260 ms | Max ~fél szívverés átlagos hiba |

