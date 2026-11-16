## Timestamp resolution

`ts` is stamped by the host daemon when a serial read returns, not by the camera. Distinct rx timestamps: 401416; gap between consecutive batches (gaps < 2 s): p10 0.201 s, p50 0.402 s, p90 0.602 s, p99 0.609 s; lines per batch p50 1, p99 19. So a single event is known to about -0.4/+0 s (it happened some time in the batch interval before its stamp) and an interval between two events has an error of about +/-0.4 s (one batch each end), more when the UART is saturated by Canon spam (115200 baud = 11.5 kB/s; backlog delays everything behind it).

## Measured copy and write intervals (raw_vidx)

| attempt # | boot | what | amount | interval s | MB/s (point) | MB/s range for +/-1 batch | frames/s |
|---|---|---|---|---|---|---|---|
| 40 | 211b (db 212) | copy (give sem x5) | 4 x 2809856 B | 1.81 | 6.2 | 5.1 .. 8.0 | 2.22 |
| 40 | 211b (db 212) | write (writing -> FIO_WriteFile done) | 14049920 B | 0.40 | 35.0 | 17.5 .. 281.0 | - |
| 42 | 214c (db 215) | copy (give sem x5) | 4 x 2809856 B | 1.81 | 6.2 | 5.1 .. 8.0 | 2.21 |
| 42 | 214c (db 215) | write (writing -> FIO_WriteFile done) | 14049920 B | 0.40 | 35.0 | 17.5 .. 281.0 | - |
| 43 | 217 (db 217) | copy (give sem x5) | 4 x 2809856 B | 2.01 | 5.6 | 4.7 .. 7.0 | 1.99 |
| 43 | 217 (db 217) | write (writing -> FIO_WriteFile done) | 14049920 B | 0.20 | 69.9 | 23.3 .. 281.0 | - |
| 45 | 224c (db 224) | copy (give sem x5) | 4 x 2809856 B | 2.01 | 5.6 | 4.7 .. 7.0 | 1.99 |
| 47 | 366b (db 367) | copy (give sem x5) | 4 x 2809856 B | 2.21 | 5.1 | 4.3 .. 6.2 | 1.81 |

Pooled copy cadence: 20 frame intervals in 9.84 s = 2.03 frames/s logged cadence.
Pooled write: 42149760 B in 1.00 s = 42.0 MB/s (3 writes).
