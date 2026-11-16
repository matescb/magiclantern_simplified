## Assert sites (file:line decimal, raw hex in brackets)

| site | message | lines | boots | first | last | last ML phase before first hit in boot (boots) | build epochs (lines) | boot ids |
|---|---|---|---|---|---|---|---|---|
| <garbled/truncated ASSERT line> |  | 212 | 63 | 07-14 | 07-22 | big.8 x52, big.5 x4, big.2 x2 (+4 more) | ax256448 x182, ax257760 x20, ax256320 x4 (+5 more) | 63 boots |
| SystemIF::KerTask.c:412 (L:0000019C) | 0 x200 | 200 | 40 | 07-18 | 07-22 | big.4a2 x29, big.4a x11 | ax257760 x95, ax256992 x65, ax257696 x20 (+2 more) | 141,142b,149,172,177e,180,186b,187d,187e,197b,200,216,223,224b,225,231,233,233c,248b,249b,250b,253c,255b,257b,259,261c,263-264,267,269-270,273-274,279,282,284,286,348b,362,365b |
| AutoExposure::AEWBInteg.c:290 (L:00000122) | nullptr x354 | 354 | 30 | 07-14 | 07-19 | big.8 x15, big.8 +rec x10, big.7 x4 (+1 more) | ax256320 x185, ax256992 x52, ax257376 x48 (+4 more) | 30b,33d,37,39c,42b,43,52,54b,55,58-59,64d,71,75,78,79b,146,148,153,153b,164c,199,226c,241,276,280,283,287,302b,304 |
| AutoExposure::AEWBInteg.c:336 (L:00000150) | nullptr x174 | 174 | 30 | 07-14 | 07-19 | big.8 x15, big.8 +rec x10, big.7 x4 (+1 more) | ax256320 x93, ax257376 x24, ax256992 x24 (+4 more) | 30b,33d,37,39c,42b,43,52,54b,55,58-59,64d,71,75,78,79b,146,148,153,153b,164c,199,226c,241,276,280,283,287,302b,304 |
| AutoExposure::AEWBInteg.c:248 (L:000000F8) | nullptr x343 | 343 | 30 | 07-14 | 07-19 | big.8 x15, big.8 +rec x10, big.7 x4 (+1 more) | ax256320 x183, ax257376 x48, ax256992 x48 (+4 more) | 30b,33d,37,39c,42b,43,52,54b,55,58-59,64d,71,75,78,79b,146,148,153,153b,164c,199,226c,241,276,280,283,287,302b,304 |
| Engine::Edmac.c:1904 (L:00000770) | nullptr x21 | 21 | 21 | 07-14 | 07-18 | big.8 +rec x11, big.8 x10 | ax256320 x17, ax257376 x4 | 30b,33d,37,39c,42b,43,47c,52,54b,55,58-59,61b,68,75,78,79b,146,148,153,153b |
| Engine::Outer.c:205 (L:000000CD) | nullptr x20 | 20 | 20 | 07-14 | 07-18 | big.8 +rec x11, big.8 x9 | ax256320 x16, ax257376 x4 | 33d,37,39c,42b,43,47c,52,54b,55,58-59,61b,68,75,78,79b,146,148,153,153b |
| ImageController::ImageController.c:931 (L:000003A3) | nullptr x18 | 18 | 18 | 07-18 | 07-20 | big.8 x12, big.7 x5, big.8 +rec x1 | ax256448 x10, ax256992 x4, ax257760 x2 (+2 more) | 164c,199,207,226c,276,280,283,287,291,295,302b,304,304b,319,322,324-325,329 |
| System::Memory.c:253 (L:000000FD) | nullptr x7 | 7 | 7 | 07-18 | 07-22 | big.4a2 x5, big.4a x2 | ax257760 x4, ax257120 x1, ax257696 x1 (+1 more) | 135,175,201,240b,252,268,364b |
| Property::PropertyMgr.c:554 (L:0000022A) | nullptr x7 | 7 | 7 | 07-18 | 07-22 | big.4a2 x5, big.4a x2 | ax257760 x4, ax257120 x1, ax257696 x1 (+1 more) | 135,175,201,240b,252,268,364b |
| Engine::MvRawProDs.c:293 (L:00000125) | nullptr x12 | 12 | 6 | 07-16 | 07-19 | big.8 +rec x6 | ax257760 x10, ax256320 x1, ax257696 x1 | 111b,159,203,209,213,220b |
| Engine::RawPro.c:452 (L:000001C4) | nullptr x12 | 12 | 6 | 07-16 | 07-19 | big.8 +rec x6 | ax257760 x10, ax256320 x1, ax257696 x1 | 111b,159,203,209,213,220b |
| SystemIF::KerInt.c:113 (L:00000071) | 0 x8 | 8 | 4 | 07-14 | 07-19 | big.7 x3, big.8 x1 | ax256992 x4, ax256320 x2, ax256448 x2 | 51b,272,275,297b |
| LiveViewCore::LvJob.c:213 (L:000000D5) | nullptr x9 | 9 | 4 | 07-19 | 07-19 | big.8 +rec x4 | ax257760 x9 | 203,209,213,220b |
| LiveViewDrive::EvfDevState.c:372 (L:00000174) | nullptr x96 | 96 | 4 | 07-19 | 07-19 | big.8 +rec x4 | ax257760 x96 | 211b,214c,217,224c |
| LiveViewDrive::EvfDevState.c:542 (L:0000021E) | nullptr x32 | 32 | 4 | 07-19 | 07-19 | big.8 +rec x4 | ax257760 x32 | 211b,214c,217,224c |
| Engine::Edmac.c:1502 (L:000005DE) | nullptr x3 | 3 | 3 | 07-14 | 07-14 | big.8 x3 | ax256320 x3 | 30b,33d,37 |
| Engine::Edmac.c:1519 (L:000005EF) | nullptr x6 | 6 | 3 | 07-14 | 07-14 | big.8 x3 | ax256320 x6 | 30b,33d,37 |
| Engine::Edmac.c:1035 (L:0000040B) | nullptr x6 | 6 | 3 | 07-14 | 07-14 | big.8 x3 | ax256320 x6 | 30b,33d,37 |
| Engine::Edmac.c:1916 (L:0000077C) | nullptr x2 | 2 | 2 | 07-14 | 07-14 | big.8 x2 | ax256320 x2 | 30b,33d |
| Engine::Outer.c:160 (L:000000A0) | nullptr x2 | 2 | 2 | 07-14 | 07-14 | big.8 x2 | ax256320 x2 | 33d,37 |
| SystemIF::KerFlag.c:700 (L:000002BC) | 0 x2 | 2 | 2 | 07-18 | 07-19 | big.4a2 x1, big.4b x1 | ax257376 x1, ax257760 x1 | 143,241 |
| Property::PropertyMgr.c:59 (L:0000003B) | nullptr x1 | 1 | 1 | 07-14 | 07-14 | big.8 x1 | ax256320 x1 | 55b |
| System::PackMem.c:803 (L:00000323) | nullptr x3 | 3 | 1 | 07-18 | 07-18 | big.8 x1 | ax257376 x3 | 145b |
| System::PackMem.c:819 (L:00000333) | nullptr x1 | 1 | 1 | 07-18 | 07-18 | big.8 x1 | ax257376 x1 | 145b |
| System::PackMem.c:1175 (L:00000497) | nullptr x3 | 3 | 1 | 07-18 | 07-18 | big.8 x1 | ax257376 x3 | 145b |
| Device::LEDDrv.c:273 (L:00000111) | nullptr x1 | 1 | 1 | 07-18 | 07-18 | big.8 x1 | ax258400 x1 | 199 |
| Device::LEDDrv.c:301 (L:0000012D) | nullptr x1 | 1 | 1 | 07-18 | 07-18 | big.8 x1 | ax258400 x1 | 199 |
| System::Task.c:229 (L:000000E5) | nullptr x1 | 1 | 1 | 07-22 | 07-22 | big.8 +rec x1 | ax257760 x1 | 366b |

## Assert bursts (asserts < 2 s apart in one boot = one burst; grouped by the set of sites)

| burst = set of sites | bursts | boots | median lines | max lines | max duration s | first | last | ML phase at burst start | bursts within 120 s of a recording marker | abort within 5 s after | build epochs |
|---|---|---|---|---|---|---|---|---|---|---|---|
| <garbled only> | 57 | 57 | 0 | 0 | 0.0 | 07-14 | 07-22 | big.8 x47, big.5 x4, big.2 x2 (+3 more) | 1 | none x57 | ax256448 x27, ax257760 x20, ax256320 x4 (+5 more) |
| KerTask.c:412 | 40 | 40 | 5 | 5 | 0.2 | 07-18 | 07-22 | big.4a2 x29, big.4a x11 | 0 | pc=0xE02F281A lr=0xE05BB769 x40 | ax257760 x19, ax256992 x13, ax257696 x4 (+2 more) |
| AEWBInteg.c:248 + AEWBInteg.c:290 + AEWBInteg.c:336 + Edmac.c:1904 + Outer.c:205 | 15 | 15 | 32 | 32 | 1.4 | 07-14 | 07-18 | big.8 x15 | 9 | none x15 | ax256320 x11, ax257376 x4 |
| ImageController.c:931 | 9 | 9 | 1 | 1 | 1.2 | 07-19 | 07-20 | big.8 x8, big.7 x1 | 1 | none x9 | ax256448 x8, ax257760 x1 |
| AEWBInteg.c:248 + AEWBInteg.c:290 + AEWBInteg.c:336 + ImageController.c:931 | 8 | 8 | 32 | 32 | 1.2 | 07-18 | 07-19 | big.8 x4, big.7 x4 | 0 | none x8 | ax256992 x4, ax256448 x2, ax257440 x1 (+1 more) |
| Memory.c:253 + PropertyMgr.c:554 | 7 | 7 | 2 | 2 | 0.0 | 07-18 | 07-22 | big.4a2 x5, big.4a x2 | 0 | pc=0xE05D1ED0 lr=0xE05BF8AF x7 | ax257760 x4, ax257120 x1, ax257696 x1 (+1 more) |
| KerInt.c:113 | 4 | 4 | 2 | 2 | 0.0 | 07-14 | 07-19 | big.7 x3, big.8 x1 | 0 | none x4 | ax256992 x2, ax256320 x1, ax256448 x1 |
| LvJob.c:213 + MvRawProDs.c:293 + RawPro.c:452 | 4 | 4 | 7 | 9 | 0.2 | 07-19 | 07-19 | big.8 x4 | 4 | none x4 | ax257760 x4 |
| EvfDevState.c:372 + EvfDevState.c:542 | 4 | 4 | 32 | 32 | 0.4 | 07-19 | 07-19 | big.8 x4 | 4 | pc=0x00000000 lr=0xE05D1161 x3, pc=0xE05B9FDA lr=0xE05B9FDB x1 | ax257760 x4 |
| Edmac.c:1904 + Outer.c:205 | 3 | 3 | 2 | 2 | 0.0 | 07-14 | 07-15 | big.8 x3 | 2 | none x3 | ax256320 x3 |
| AEWBInteg.c:248 + AEWBInteg.c:290 + AEWBInteg.c:336 | 2 | 2 | 29 | 30 | 0.6 | 07-15 | 07-15 | big.8 x2 | 1 | none x2 | ax256320 x2 |
| MvRawProDs.c:293 + RawPro.c:452 | 2 | 2 | 2 | 2 | 0.0 | 07-16 | 07-18 | big.8 x2 | 2 | none x2 | ax256320 x1, ax257696 x1 |
| AEWBInteg.c:248 + AEWBInteg.c:290 + AEWBInteg.c:336 + Edmac.c:1035 + Edmac.c:1502 + Edmac.c:1519 + Edmac.c:1904 + Edmac.c:1916 | 1 | 1 | 32 | 32 | 1.2 | 07-14 | 07-14 | big.8 x1 | 0 | none x1 | ax256320 x1 |
| AEWBInteg.c:248 + AEWBInteg.c:290 + AEWBInteg.c:336 + Edmac.c:1035 + Edmac.c:1502 + Edmac.c:1519 + Edmac.c:1904 + Edmac.c:1916 + Outer.c:160 + Outer.c:205 | 1 | 1 | 32 | 32 | 1.2 | 07-14 | 07-14 | big.8 x1 | 0 | none x1 | ax256320 x1 |
| AEWBInteg.c:248 + AEWBInteg.c:290 + AEWBInteg.c:336 + Edmac.c:1035 + Edmac.c:1502 + Edmac.c:1519 + Edmac.c:1904 + Outer.c:160 + Outer.c:205 | 1 | 1 | 32 | 32 | 1.2 | 07-14 | 07-14 | big.8 x1 | 0 | none x1 | ax256320 x1 |
| PropertyMgr.c:59 | 1 | 1 | 1 | 1 | 0.0 | 07-14 | 07-14 | big.8 x1 | 0 | none x1 | ax256320 x1 |
| KerFlag.c:700 | 1 | 1 | 1 | 1 | 0.0 | 07-18 | 07-18 | big.4a2 x1 | 0 | pc=0x00000000 lr=0xE00432DB x1 | ax257376 x1 |
| PackMem.c:1175 + PackMem.c:803 + PackMem.c:819 | 1 | 1 | 7 | 7 | 0.0 | 07-18 | 07-18 | big.8 x1 | 0 | pc=0xE05BD5DE lr=0xE04221EF x1 | ax257376 x1 |
| AEWBInteg.c:248 + AEWBInteg.c:290 + AEWBInteg.c:336 + ImageController.c:931 + LEDDrv.c:273 + LEDDrv.c:301 | 1 | 1 | 15 | 15 | 1.0 | 07-18 | 07-18 | big.8 x1 | 0 | pc=0xE0729566 lr=0x000E8389 x1 | ax258400 x1 |
| AEWBInteg.c:248 + AEWBInteg.c:290 + AEWBInteg.c:336 + KerFlag.c:700 | 1 | 1 | 32 | 32 | 1.2 | 07-19 | 07-19 | big.4b x1 | 0 | none x1 | ax257760 x1 |
| Task.c:229 | 1 | 1 | 1 | 1 | 0.0 | 07-22 | 07-22 | big.8 x1 | 1 | pc=0xE005DFA2 lr=0xE005DFA1 x1 | ax257760 x1 |
