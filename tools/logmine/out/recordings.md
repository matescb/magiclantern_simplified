## Recording attempts (47)

`frames` for raw_vidx = max(count of `cbr_w: give sem`, distinct `wk.dst`) - a LOWER bound, the module logs are rate-limited from build 81fd5d54 on. For mlv_lite = count of `@ML> framewrite-ok`. `elapsed` = first to last marker of the attempt; every timestamp is the host arrival time of a ~0.2 s serial read batch (see rates.md).

| # | start UTC | cold-start boot (mllog db boot) | core build | module | frames (logged) | frame bytes | bytes written | elapsed s | how it ended |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 07-14 22:13:47 | 58 (db 58) | ax256320 | mlv_lite | 0 | - | hdr 0 B; framewrite-ok x0 (0 B) | 0.0 | asserts AEWBInteg.c x30,Edmac.c x1,Outer.c x1; no abort; boot log continues 151s (69 heartbeats later) |
| 2 | 07-14 22:19:27 | 59 (db 59) | ax256320 | mlv_lite | 0 | - | hdr 0 B; framewrite-ok x0 (0 B) | 0.0 | asserts AEWBInteg.c x30,Edmac.c x1,Outer.c x1; no abort; boot log continues 78s (57 heartbeats later) |
| 3 | 07-15 04:42:27 | 61b (db 61) | ax256320 | mlv_lite | 0 | - | hdr 0 B; framewrite-ok x0 (0 B) | 0.0 | asserts Edmac.c x1,Outer.c x1; no abort; boot log continues 12473s (93 heartbeats later) |
| 4 | 07-15 08:26:44 | 63b (db 63) | ax256320 | mlv_lite | 0 | - | hdr 0 B; framewrite-ok x0 (0 B) | 0.0 | no abort; boot log continues 164s (81 heartbeats later) |
| 5 | 07-15 09:40:10 | 67 (db 67) | ax256320 | mlv_lite | 0 | - | hdr 0 B; framewrite-ok x0 (0 B) | 0.0 | no abort; boot log continues 2729s (101 heartbeats later) |
| 6 | 07-15 10:27:49 | 68 (db 68) | ax256320 | mlv_lite | 0 | - | hdr 0 B; framewrite-ok x0 (0 B) | 0.0 | asserts Edmac.c x1,Outer.c x1; no abort; boot log continues 94s (89 heartbeats later) |
| 7 | 07-15 10:38:44 | 71 (db 71) | ax256320 | mlv_lite | 0 | - | hdr 0 B; framewrite-ok x0 (0 B) | 0.0 | asserts AEWBInteg.c x30; no abort; boot log continues 75s (60 heartbeats later) |
| 8 | 07-15 11:01:04 | 75 (db 76) | ax256320 | mlv_lite | 0 | - | hdr 0 B; framewrite-ok x0 (0 B) | 0.0 | asserts AEWBInteg.c x30,Edmac.c x1,Outer.c x1; no abort; boot log continues 235s (238 heartbeats later) |
| 9 | 07-15 11:06:58 | 78 (db 78) | ax256320 | mlv_lite | 0 | - | hdr 572 B; framewrite-ok x0 (0 B) | 0.6 | asserts AEWBInteg.c x30,Edmac.c x1,Outer.c x1; no abort; boot log continues 561s (103 heartbeats later) |
| 10 | 07-15 11:18:30 | 79b (db 79) | ax256320 | mlv_lite | 0 | - | hdr 572 B; framewrite-ok x0 (0 B) | 1.0 | asserts AEWBInteg.c x30,Edmac.c x1,Outer.c x1; no abort; boot log continues 216s (96 heartbeats later) |
| 11 | 07-15 11:32:10 | 81 (db 81) | ax256320 | mlv_lite | 0 | - | hdr 572 B; framewrite-ok x0 (0 B) | 0.6 | no abort; boot log continues 35907s (94 heartbeats later) |
| 12 | 07-15 23:43:00 | 87 (db 87) | ax256320 | mlv_lite | 0 | - | hdr 0 B; framewrite-ok x0 (0 B) | 0.0 | no abort; boot log continues 32345s (82 heartbeats later) |
| 13 | 07-16 11:12:54 | 111b (db 112) | ax256320 | mlv_lite | 0 | - | hdr 0 B; framewrite-ok x0 (0 B) | 0.2 | MvRawPro/RawPro/LvJob storm x2; no abort; NO heartbeat after +15s (boot log ends 40784s later: hang or power-off) |
| 14 | 07-18 14:05:16 | 146 (db 146) | ax257376 | mlv_lite | 0 | - | hdr 572 B; framewrite-ok x0 (0 B) | 0.6 | asserts AEWBInteg.c x30,Edmac.c x1,Outer.c x1; no abort; boot log continues 755s (115 heartbeats later) |
| 15 | 07-18 14:21:03 | 148 (db 149) | ax257376 | mlv_lite | 1 | - | hdr 572 B; framewrite-ok x1 (430592 B) | 2.8 | asserts AEWBInteg.c x30,Edmac.c x1,Outer.c x1; no abort; boot log continues 119s (114 heartbeats later) |
| 16 | 07-18 14:47:40 | 153 (db 153) | ax257376 | mlv_lite | 1 | - | hdr 572 B; framewrite-ok x1 (430592 B) | 1.2 | asserts AEWBInteg.c x30,Edmac.c x1,Outer.c x1; no abort; boot log continues 104s (92 heartbeats later) |
| 17 | 07-18 14:56:58 | 153b (db 154) | ax257376 | mlv_lite | 1 | - | hdr 572 B; framewrite-ok x1 (430592 B) | 3.0 | asserts AEWBInteg.c x30,Edmac.c x1,Outer.c x1; no abort; boot log continues 700s (133 heartbeats later) |
| 18 | 07-18 15:36:21 | 159 (db 159) | ax257696 | mlv_lite | 1 | - | hdr 572 B; framewrite-ok x1 (430592 B) | 8.0 | no abort; boot log continues 84s (88 heartbeats later) |
| 19 | 07-18 15:37:27 | 159 (db 159) | ax257696 | mlv_lite | 0 | - | hdr 572 B; framewrite-ok x0 (0 B) | 2.4 | MvRawPro/RawPro/LvJob storm x2; no abort; NO heartbeat after +15s (boot log ends 24s later: hang or power-off) |
| 20 | 07-18 17:13:02 | 162 (db 162) | ax257440 | mlv_lite | 0 | - | hdr 572 B; framewrite-ok x0 (0 B) | 2.4 | no abort; boot log continues 567s (619 heartbeats later) |
| 21 | 07-18 17:18:35 | 162 (db 162) | ax257440 | mlv_lite | 0 | - | hdr 572 B; framewrite-ok x0 (0 B) | 1.4 | no abort; NO heartbeat after +15s (boot log ends 235s later: hang or power-off) |
| 22 | 07-18 17:39:35 | 165b (db 167) | ax257440 | mlv_lite | 1 | - | hdr 572 B; framewrite-ok x1 (3796992 B) | 8.2 | no abort; boot log continues 966s (500 heartbeats later) |
| 23 | 07-18 17:57:42 | 169 (db 170) | ax257696 | mlv_lite | 1 | - | hdr 572 B; framewrite-ok x1 (3796992 B) | 8.4 | no abort; boot log continues 1200s (930 heartbeats later) |
| 24 | 07-18 18:53:51 | 177f (db 177) | ax257696 | mlv_lite | 1 | - | hdr 572 B; framewrite-ok x1 (2712576 B) | 8.2 | no abort; boot log continues 474s (458 heartbeats later) |
| 25 | 07-18 18:56:39 | 177f (db 177) | ax257696 | mlv_lite | 0 | - | hdr 572 B; framewrite-ok x0 (0 B) | 3.4 | no abort; boot log continues 311s (145 heartbeats later) |
| 26 | 07-18 18:58:10 | 177f (db 177) | ax257696 | mlv_lite | 0 | - | hdr 572 B; framewrite-ok x0 (0 B) | 2.2 | no abort; NO heartbeat after +15s (boot log ends 221s later: hang or power-off) |
| 27 | 07-18 19:04:22 | 179b (db 179) | ax257696 | mlv_lite | 1 | - | hdr 1144 B; framewrite-ok x1 (2712576 B) | 18.3 | no abort; NO heartbeat after +15s (boot log ends 376s later: hang or power-off) |
| 28 | 07-18 19:13:29 | 180c (db 181) | ax257696 | mlv_lite | 1 | - | hdr 572 B; framewrite-ok x1 (3796992 B) | 8.8 | no abort; boot log continues 3400s (1060 heartbeats later) |
| 29 | 07-18 20:13:00 | 182b (db 184) | ax257696 | mlv_lite | 1 | - | hdr 572 B; framewrite-ok x1 (3796992 B) | 9.2 | no abort; boot log continues 259s (222 heartbeats later) |
| 30 | 07-18 20:15:17 | 182b (db 184) | ax257696 | mlv_lite | 0 | - | hdr 572 B; framewrite-ok x0 (0 B) | 2.2 | no abort; NO heartbeat after +15s (boot log ends 129s later: hang or power-off) |
| 31 | 07-18 20:38:07 | 188 (db 188) | ax257760 | mlv_lite | 1 | - | hdr 572 B; framewrite-ok x1 (3796992 B) | 8.4 | no abort; boot log continues 3057s (975 heartbeats later) |
| 32 | 07-18 20:46:50 | 188 (db 189) | ax257760 | mlv_lite | 0 | - | hdr 572 B; framewrite-ok x0 (0 B) | 3.0 | no abort; NO heartbeat after +15s (boot log ends 2540s later: hang or power-off) |
| 33 | 07-18 22:40:35 | 195b (db 196) | ax257760 | raw_vidx | 0 | - | - | 0.0 | no abort; boot log continues 2680s (48 heartbeats later) |
| 34 | 07-18 22:41:10 | 195b (db 196) | ax257760 | raw_vidx | 0 | - | - | 6.8 | no abort; NO heartbeat after +15s (boot log ends 2638s later: hang or power-off); log errors: worker couldn't take edmac_s x9, crop_buf was  x159 |
| 35 | 07-19 00:05:55 | 203 (db 203) | ax257760 | raw_vidx | 0 | - | - | 0.4 | MvRawPro/RawPro/LvJob storm x8; no abort; NO heartbeat after +15s (boot log ends 1168s later: hang or power-off) |
| 36 | 07-19 00:29:27 | 205 (db 206) | ax257760 | raw_vidx | 1 | 2809856 | - | 20.3 | no abort; boot log continues 12559s (119 heartbeats later); log errors: worker couldn't take edmac_s x9, crop_buf was  x497 |
| 37 | 07-19 04:00:29 | 207 (db 207) | ax257760 | raw_vidx | 0 | - | - | 0.0 | asserts ImageController.c x1; no abort; NO heartbeat after +15s (boot log ends 115s later: hang or power-off) |
| 38 | 07-19 04:05:16 | 209 (db 209) | ax257760 | raw_vidx | 0 | - | - | 0.2 | MvRawPro/RawPro/LvJob storm x6; no abort; NO heartbeat after +15s (boot log ends 115s later: hang or power-off) |
| 39 | 07-19 04:07:51 | 210 (db 210) | ax257760 | raw_vidx | 0 | - | - | 0.0 | ABORT pc=0x000E855E lr=0xE016402F 0.0s after last marker |
| 40 | 07-19 04:23:07 | 211b (db 212) | ax257760 | raw_vidx | 5 | 2809856 | 14049920 B in 0.4s | 2.8 | queue full -> self-stop; EvfDevState nullptr x32; ABORT pc=0x00000000 lr=0xE05D1161 0.4s after last marker |
| 41 | 07-19 04:26:28 | 213 (db 213) | ax257760 | raw_vidx | 0 | - | - | 0.0 | MvRawPro/RawPro/LvJob storm x6; no abort; NO heartbeat after +15s (boot log ends 22777s later: hang or power-off) |
| 42 | 07-19 10:50:26 | 214c (db 215) | ax257760 | raw_vidx | 5 | 2809856 | 14049920 B in 0.4s | 2.8 | queue full -> self-stop; EvfDevState nullptr x32; ABORT pc=0x00000000 lr=0xE05D1161 0.4s after last marker |
| 43 | 07-19 10:56:39 | 217 (db 217) | ax257760 | raw_vidx | 5 | 2809856 | 14049920 B in 0.2s | 2.8 | queue full -> self-stop; EvfDevState nullptr x32; ABORT pc=0xE05B9FDA lr=0xE05B9FDB 0.6s after last marker |
| 44 | 07-19 12:22:49 | 220b (db 220) | ax257760 | raw_vidx | 0 | - | - | 0.0 | MvRawPro/RawPro/LvJob storm x9; no abort; NO heartbeat after +15s (boot log ends 40s later: hang or power-off) |
| 45 | 07-19 13:07:38 | 224c (db 224) | ax257760 | raw_vidx | 5 | 2809856 | - | 2.6 | READY-poll ok; queue full -> self-stop; EvfDevState nullptr x32; ABORT pc=0x00000000 lr=0xE05D1161 0.8s after last marker |
| 46 | 07-19 13:25:22 | 226c (db 226) | ax257760 | raw_vidx | 0 | - | - | 1.2 | no abort; boot log continues 6713s (330 heartbeats later) |
| 47 | 07-22 00:58:42 | 366b (db 367) | ax257760 | raw_vidx | 5 | 2809856 | - | 2.8 | READY-poll ok; asserts Task.c x1; ABORT pc=0xE005DFA2 lr=0xE005DFA1 0.2s after last marker |
