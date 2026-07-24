# Synsigra Verification Results — Per-Case Verdicts

Generated: 2026-07-24  
Algorithm: rspt_module v0.1.0 (bc8d1a3)  
Verifier: synsigra 0.14.0

<style>
table { border-collapse: collapse; font-size: 13px; margin: 20px 0; }
th, td { border: 1px solid #d8dee8; padding: 6px 10px; text-align: center; vertical-align: middle; }
th { background: #eef2f7; font-size: 11px; white-space: nowrap; }
td:first-child { text-align: left; }
td:last-child { text-align: center; font-weight: bold; }
small { color: #5f6b7a; }
tr:hover { background: #f6f8fb; }
</style>

<h2>r_peak_rr_simple_stress_v1 v1.0 — R-peak + RR Simple Stress</h2>
<p><strong>Evidence verdict: PASSED</strong></p>
<table>
<thead><tr>
<th>Case</th>
<th>Rpk F1</th>
<th>Rpk Se</th>
<th>Rpk PPV</th>
<th>Rpk tMAE</th>
<th>RR cover</th>
<th>RR pred</th>
<th>RR stat</th>
<th>RR tol%</th>
<th>RR MAE</th>
<th>RR P95</th>
<th>Case verdict</th>
</tr></thead>
<tbody>
<tr>
<td><strong>clean_70</strong></td>
<td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>2.8 ms</strong><br><small>≤ 20 ms ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 90% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>1.0 ms</strong><br><small>≤ 25 ms ✅</small></td><td style="color:#176b45"><strong>1.1 ms</strong><br><small>≤ 50 ms ✅</small></td>
<td style="color:#176b45"><strong>✅ PASS</strong></td>
</tr>
<tr>
<td><strong>slow_45</strong></td>
<td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>2.7 ms</strong><br><small>≤ 20 ms ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 90% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>1.0 ms</strong><br><small>≤ 25 ms ✅</small></td><td style="color:#176b45"><strong>1.3 ms</strong><br><small>≤ 50 ms ✅</small></td>
<td style="color:#176b45"><strong>✅ PASS</strong></td>
</tr>
<tr>
<td><strong>fast_120</strong></td>
<td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>3.9 ms</strong><br><small>≤ 20 ms ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 90% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>0.1 ms</strong><br><small>≤ 25 ms ✅</small></td><td style="color:#176b45"><strong>0.0 ms</strong><br><small>≤ 50 ms ✅</small></td>
<td style="color:#176b45"><strong>✅ PASS</strong></td>
</tr>
<tr>
<td><strong>baseline_powerline</strong></td>
<td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 80% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 75% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 75% ✅</small></td><td style="color:#176b45"><strong>3.0 ms</strong><br><small>≤ 40 ms ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 75% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 75% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 90% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 75% ✅</small></td><td style="color:#176b45"><strong>0.7 ms</strong><br><small>≤ 25 ms ✅</small></td><td style="color:#176b45"><strong>1.7 ms</strong><br><small>≤ 50 ms ✅</small></td>
<td style="color:#176b45"><strong>✅ PASS</strong></td>
</tr>
</tbody></table>
<p><em>4/4 cases passed</em></p>

---

<h2>r_peak_rr_snr_ladder_v1 v1.0 — R-peak + RR SNR Ladder</h2>
<p><strong>Evidence verdict: FAILED</strong></p>
<table>
<thead><tr>
<th>Case</th>
<th>Rpk F1</th>
<th>Rpk Se</th>
<th>Rpk PPV</th>
<th>Rpk tMAE</th>
<th>RR cover</th>
<th>RR pred</th>
<th>RR stat</th>
<th>RR tol%</th>
<th>RR MAE</th>
<th>RR P95</th>
<th>Case verdict</th>
</tr></thead>
<tbody>
<tr>
<td><strong>clean</strong></td>
<td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>3.8 ms</strong><br><small>≤ 20 ms ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 90% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 98% ✅</small></td><td style="color:#176b45"><strong>2.2 ms</strong><br><small>≤ 25 ms ✅</small></td><td style="color:#176b45"><strong>9.2 ms</strong><br><small>≤ 50 ms ✅</small></td>
<td style="color:#176b45"><strong>✅ PASS</strong></td>
</tr>
<tr>
<td><strong>snr_m1</strong></td>
<td style="color:#176b45"><strong>98.7%</strong><br><small>≥ 95% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 92% ✅</small></td><td style="color:#176b45"><strong>97.5%</strong><br><small>≥ 92% ✅</small></td><td style="color:#176b45"><strong>6.5 ms</strong><br><small>≤ 40 ms ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 92% ✅</small></td><td style="color:#176b45"><strong>97.5%</strong><br><small>≥ 92% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 90% ✅</small></td><td style="color:#a12828"><strong>72.7%</strong><br><small>≥ 92% ❌</small></td><td style="color:#a12828"><strong>43.2 ms</strong><br><small>≤ 25 ms ❌</small></td><td style="color:#a12828"><strong>340.7 ms</strong><br><small>≤ 50 ms ❌</small></td>
<td style="color:#a12828"><strong>❌ FAIL</strong></td>
</tr>
<tr>
<td><strong>snr_m2</strong></td>
<td style="color:#176b45"><strong>96.2%</strong><br><small>≥ 93% ✅</small></td><td style="color:#176b45"><strong>98.7%</strong><br><small>≥ 90% ✅</small></td><td style="color:#176b45"><strong>93.9%</strong><br><small>≥ 90% ✅</small></td><td style="color:#176b45"><strong>7.2 ms</strong><br><small>≤ 40 ms ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 90% ✅</small></td><td style="color:#176b45"><strong>95.1%</strong><br><small>≥ 90% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 90% ✅</small></td><td style="color:#a12828"><strong>20.8%</strong><br><small>≥ 90% ❌</small></td><td style="color:#a12828"><strong>140.7 ms</strong><br><small>≤ 25 ms ❌</small></td><td style="color:#a12828"><strong>521.2 ms</strong><br><small>≤ 50 ms ❌</small></td>
<td style="color:#a12828"><strong>❌ FAIL</strong></td>
</tr>
<tr>
<td><strong>snr_m3</strong></td>
<td style="color:#176b45"><strong>93.3%</strong><br><small>≥ 90% ✅</small></td><td style="color:#176b45"><strong>98.7%</strong><br><small>≥ 85% ✅</small></td><td style="color:#176b45"><strong>88.5%</strong><br><small>≥ 85% ✅</small></td><td style="color:#176b45"><strong>9.2 ms</strong><br><small>≤ 40 ms ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 85% ✅</small></td><td style="color:#176b45"><strong>89.5%</strong><br><small>≥ 85% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 90% ✅</small></td><td style="color:#a12828"><strong>18.2%</strong><br><small>≥ 85% ❌</small></td><td style="color:#a12828"><strong>170.9 ms</strong><br><small>≤ 25 ms ❌</small></td><td style="color:#a12828"><strong>547.6 ms</strong><br><small>≤ 50 ms ❌</small></td>
<td style="color:#a12828"><strong>❌ FAIL</strong></td>
</tr>
<tr>
<td><strong>snr_m4</strong></td>
<td style="color:#a12828"><strong>81.7%</strong><br><small>≥ 85% ❌</small></td><td style="color:#176b45"><strong>88.5%</strong><br><small>≥ 80% ✅</small></td><td style="color:#a12828"><strong>75.8%</strong><br><small>≥ 80% ❌</small></td><td style="color:#176b45"><strong>7.8 ms</strong><br><small>≤ 40 ms ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 80% ✅</small></td><td style="color:#176b45"><strong>85.6%</strong><br><small>≥ 80% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 90% ✅</small></td><td style="color:#a12828"><strong>11.7%</strong><br><small>≥ 80% ❌</small></td><td style="color:#a12828"><strong>201.9 ms</strong><br><small>≤ 25 ms ❌</small></td><td style="color:#a12828"><strong>596.6 ms</strong><br><small>≤ 50 ms ❌</small></td>
<td style="color:#a12828"><strong>❌ FAIL</strong></td>
</tr>
<tr>
<td><strong>snr_m5</strong></td>
<td style="color:#176b45"><strong>80.5%</strong><br><small>≥ 78% ✅</small></td><td style="color:#176b45"><strong>87.2%</strong><br><small>≥ 73% ✅</small></td><td style="color:#176b45"><strong>74.7%</strong><br><small>≥ 73% ✅</small></td><td style="color:#176b45"><strong>9.0 ms</strong><br><small>≤ 40 ms ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 73% ✅</small></td><td style="color:#176b45"><strong>85.6%</strong><br><small>≥ 73% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 90% ✅</small></td><td style="color:#a12828"><strong>10.4%</strong><br><small>≥ 73% ❌</small></td><td style="color:#a12828"><strong>208.2 ms</strong><br><small>≤ 25 ms ❌</small></td><td style="color:#a12828"><strong>563.1 ms</strong><br><small>≤ 50 ms ❌</small></td>
<td style="color:#a12828"><strong>❌ FAIL</strong></td>
</tr>
<tr>
<td><strong>snr_m6</strong></td>
<td style="color:#176b45"><strong>76.6%</strong><br><small>≥ 74% ✅</small></td><td style="color:#176b45"><strong>82.1%</strong><br><small>≥ 69% ✅</small></td><td style="color:#176b45"><strong>71.9%</strong><br><small>≥ 69% ✅</small></td><td style="color:#176b45"><strong>9.1 ms</strong><br><small>≤ 40 ms ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 69% ✅</small></td><td style="color:#176b45"><strong>87.5%</strong><br><small>≥ 69% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 90% ✅</small></td><td style="color:#a12828"><strong>3.9%</strong><br><small>≥ 69% ❌</small></td><td style="color:#a12828"><strong>214.9 ms</strong><br><small>≤ 25 ms ❌</small></td><td style="color:#a12828"><strong>552.8 ms</strong><br><small>≤ 50 ms ❌</small></td>
<td style="color:#a12828"><strong>❌ FAIL</strong></td>
</tr>
<tr>
<td><strong>snr_m7</strong></td>
<td style="color:#176b45"><strong>71.1%</strong><br><small>≥ 70% ✅</small></td><td style="color:#176b45"><strong>75.6%</strong><br><small>≥ 65% ✅</small></td><td style="color:#176b45"><strong>67.0%</strong><br><small>≥ 65% ✅</small></td><td style="color:#176b45"><strong>8.4 ms</strong><br><small>≤ 40 ms ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 65% ✅</small></td><td style="color:#176b45"><strong>88.5%</strong><br><small>≥ 65% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 90% ✅</small></td><td style="color:#a12828"><strong>10.4%</strong><br><small>≥ 65% ❌</small></td><td style="color:#a12828"><strong>194.0 ms</strong><br><small>≤ 25 ms ❌</small></td><td style="color:#a12828"><strong>511.0 ms</strong><br><small>≤ 50 ms ❌</small></td>
<td style="color:#a12828"><strong>❌ FAIL</strong></td>
</tr>
<tr>
<td><strong>snr_m8</strong></td>
<td style="color:#176b45"><strong>66.3%</strong><br><small>≥ 65% ✅</small></td><td style="color:#176b45"><strong>70.5%</strong><br><small>≥ 60% ✅</small></td><td style="color:#176b45"><strong>62.5%</strong><br><small>≥ 60% ✅</small></td><td style="color:#176b45"><strong>7.0 ms</strong><br><small>≤ 40 ms ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 60% ✅</small></td><td style="color:#176b45"><strong>88.5%</strong><br><small>≥ 60% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 90% ✅</small></td><td style="color:#a12828"><strong>7.8%</strong><br><small>≥ 60% ❌</small></td><td style="color:#a12828"><strong>204.9 ms</strong><br><small>≤ 25 ms ❌</small></td><td style="color:#a12828"><strong>517.0 ms</strong><br><small>≤ 50 ms ❌</small></td>
<td style="color:#a12828"><strong>❌ FAIL</strong></td>
</tr>
<tr>
<td><strong>snr_m9</strong></td>
<td style="color:#176b45"><strong>63.9%</strong><br><small>≥ 62% ✅</small></td><td style="color:#176b45"><strong>67.9%</strong><br><small>≥ 58% ✅</small></td><td style="color:#176b45"><strong>60.2%</strong><br><small>≥ 58% ✅</small></td><td style="color:#176b45"><strong>5.8 ms</strong><br><small>≤ 40 ms ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 58% ✅</small></td><td style="color:#176b45"><strong>88.5%</strong><br><small>≥ 58% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 90% ✅</small></td><td style="color:#a12828"><strong>7.8%</strong><br><small>≥ 58% ❌</small></td><td style="color:#a12828"><strong>205.2 ms</strong><br><small>≤ 25 ms ❌</small></td><td style="color:#a12828"><strong>521.2 ms</strong><br><small>≤ 50 ms ❌</small></td>
<td style="color:#a12828"><strong>❌ FAIL</strong></td>
</tr>
<tr>
<td><strong>snr_m10</strong></td>
<td style="color:#176b45"><strong>63.5%</strong><br><small>≥ 60% ✅</small></td><td style="color:#176b45"><strong>67.9%</strong><br><small>≥ 55% ✅</small></td><td style="color:#176b45"><strong>59.6%</strong><br><small>≥ 55% ✅</small></td><td style="color:#176b45"><strong>6.0 ms</strong><br><small>≤ 40 ms ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 55% ✅</small></td><td style="color:#176b45"><strong>87.5%</strong><br><small>≥ 55% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 90% ✅</small></td><td style="color:#a12828"><strong>7.8%</strong><br><small>≥ 55% ❌</small></td><td style="color:#a12828"><strong>205.4 ms</strong><br><small>≤ 25 ms ❌</small></td><td style="color:#a12828"><strong>525.2 ms</strong><br><small>≤ 50 ms ❌</small></td>
<td style="color:#a12828"><strong>❌ FAIL</strong></td>
</tr>
<tr>
<td><strong>snr_m11</strong></td>
<td style="color:#176b45"><strong>61.8%</strong><br><small>≥ 55% ✅</small></td><td style="color:#176b45"><strong>65.4%</strong><br><small>≥ 50% ✅</small></td><td style="color:#176b45"><strong>58.6%</strong><br><small>≥ 50% ✅</small></td><td style="color:#176b45"><strong>6.4 ms</strong><br><small>≤ 40 ms ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 50% ✅</small></td><td style="color:#176b45"><strong>89.5%</strong><br><small>≥ 50% ✅</small></td><td style="color:#176b45"><strong>100.0%</strong><br><small>≥ 90% ✅</small></td><td style="color:#a12828"><strong>6.5%</strong><br><small>≥ 50% ❌</small></td><td style="color:#a12828"><strong>219.4 ms</strong><br><small>≤ 30 ms ❌</small></td><td style="color:#a12828"><strong>581.2 ms</strong><br><small>≤ 60 ms ❌</small></td>
<td style="color:#a12828"><strong>❌ FAIL</strong></td>
</tr>
</tbody></table>
<p><em>1/12 cases passed</em></p>