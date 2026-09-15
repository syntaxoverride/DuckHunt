# Customizing answers and flags

Flags and keywords live in **firmware source**, not on the display assets.  
Change the text → **recompile → reflash** the board. You do **not** need to regenerate GIFs or other README images.

There is no in-game UI on the CYD / ESP32-S3 for editing puzzles (by design). Staff change values in the repo, then flash.

---

## What to edit

### CYD (per-duck flock, serials 1–8)

| What | File | Symbol |
|------|------|--------|
| Board number | [`CYD/duck_identity.h`](CYD/duck_identity.h) | `DUCK_SERIAL` (`1`…`8`) |
| Stories, answers, flags, stage-3 colour | [`CYD/CYD.ino`](CYD/CYD.ino) | `GAMES_BY_DUCK[…]` |
| Staff answer sheet / `--list` | [`flash_duck.py`](flash_duck.py) | `DUCKS` list (**keep in sync** with `GAMES_BY_DUCK`) |

Each duck has **Game 1** and **Game 2**. Each game has three stages:

1. Plain keyword → `answer[0]` / `flag[0]`
2. Usually Base64 in the **story** → decoded word in `answer[1]` / `flag[1]`
3. Must be on a flock colour (`colour_need`) + keyword → `answer[2]` / `flag[2]`

Example (Duck 1, Game 1) — change any of these strings together so the story still matches the answer:

```c
{ "QUACK", "POND", "HOME" },
{ "WiCyS{d01_g1_quack}", "WiCyS{d01_g1_pond}", "WiCyS{d01_g1_home}" },
"yellow",
```

If you change stage 2’s plaintext (e.g. `POND` → `LAKE`), also update:

- the Base64 blob **inside the story string** (`UE9ORA==` → `b64(LAKE)`), and  
- the matching row in `flash_duck.py` (`g1` / `g1f`).

Quick Base64 check:

```bash
printf '%s' 'LAKE' | base64
```

### ESP32-S3 AMOLED (single puzzle set)

| What | File | Symbol |
|------|------|--------|
| Stories, answers, flags | [`ESP32_S3/ESP32_S3.ino`](ESP32_S3/ESP32_S3.ino) | `GAMES[]` |

This board does **not** use `duck_identity.h` / eight serials. One firmware image = one shared answer set.

### Optional: admin PIN

Default PIN is `24650` (`ADMIN_CODE` in both sketches). Change it in source and reflash if you do not want the public default.

---

## After you edit

1. **CYD** — set `DUCK_SERIAL` (or use `./flash_duck.py N`, which patches identity and builds).
2. Compile and upload (see README build notes, or `./flash_duck.py --batch 1-8`).
3. On device: admin → **RESET BOBBERS** (or serial `R`) so old progress does not stick.
4. Spot-check with nRF Connect: STORY → SUBMIT → FLAG.

BLE UUIDs and advertising name pattern (`{Colour}-Duck-{N}`) can stay as-is. Players only need new keywords/flags if you changed those strings.

---

## Checklist when inventing a new set

- [ ] `answer[]` matches what players should type (UTF-8 text in SUBMIT)
- [ ] Stage-2 story Base64 decodes to `answer[1]`
- [ ] `colour_need` is a real tier name (`yellow`, `green`, `blue`, …) for stage 3
- [ ] Flag strings are unique per duck/stage if you score in a platform
- [ ] `flash_duck.py` `DUCKS` matches CYD (so `--list` is trustworthy)
- [ ] ESP32-S3 `GAMES[]` updated if that board is in the event
- [ ] Every affected board reflashed

---

## What you do *not* need to change

- `assets/*.gif` / mockups — cosmetics only  
- `WiCyS_BLE_Talking_to_Ducks.html` — unless you want the handout examples to match new keywords  
- BLE service/characteristic UUIDs — unless you intentionally fork the protocol  
