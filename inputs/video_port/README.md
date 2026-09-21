# Flashback: The Quest for Identity — Sega Master System Port

A complete, faithful port of Delphine Software's 1992 cinematic platformer **Flashback (The Quest for Identity)** for the **Sega Master System (SMS)**.

---

## 🌟 Features

- **4 Megabyte ROM (32 Megabit / 256 Banks × 16 KB)** using standard Sega paging mapper.
- **Full Cinematic FMV Cutscenes Engine** (55 16KB data banks) streaming:
  - Delphine Software Logo
  - Intro Part 1 (Cyberpunk pursuit and memory download)
  - Intro Part 2 (Titan crash)
  - Debut Jungle Crash Scene
  - Holographic Message Playback (Holocube)
  - Elevator Escape
  - Disintegration / Game Over
- **Complete Conrad Rotoscoped Animation Engine** with all 357 original rotoscoped animation frames across both Left and Right facing orientations (14 16KB banks).
- **Canonical Sega Genesis / Amiga Color Palette**:
  - Brown leather jacket (`0x0B`, `0x06`)
  - Blue denim jeans (`0x3A`, `0x35`, `0x25`, `0x20`)
  - Peach skin (`0x1B`)
  - Brown hair (`0x06`, `0x01`)
  - White shoes and t-shirt (`0x3F`)
  - Cyan blaster bolts & shield glow (`0x30`)
- **Full Player Moveset**:
  - Idle Stance
  - Directional Walk
  - Run Sprint with skid deceleration stop
  - Turnaround animation
  - Low crouch & stand transitions
  - Combat somersault roll
  - Vertical jump up
  - Running leap jump forward
  - Draw blaster weapon & holster
  - Armed aiming & laser projectile shooting
  - Floor item / Holocube pickup
  - Ledge climbing & drop down
  - Damage flinch and death collapse
- **8 Interactive Titan Jungle Rooms** (Rooms 26 through 33) with room switching, security drones, alien mutants, snapping plants, laser gates, switches, and recharge terminals.
- **SN76489 PSG Sound Effects**: Laser shots, footsteps, somersault swooshes, item pickups, elevator/switch chimes, and damage hits.
- **Custom SMS Controller Guide & High-Resolution Title Menu Screen**.

---

## 🎮 Controller Layout

| Input | Standing / Neutral | While Crouching | Armed Combat Stance |
|---|---|---|---|
| **D-Pad Left / Right** | Walk Left / Right | Somersault Roll | Aim Left / Right |
| **D-Pad Down** | Crouch Down | — | Crouch Aim |
| **D-Pad Up** | High Jump Up / Climb | Stand Up | — |
| **Button 1** | Floor Pickup / Action | Floor Item Pickup | Holster Blaster |
| **Button 2** | Draw Blaster Weapon | Roll Somersault | Fire Laser Bolt |
| **D-Pad + Hold 1** | Run Sprint | — | — |
| **Run + Button 2** | Running Long Leap | — | — |
| **Pause Button** | Pause / Resume | — | — |

---

## 🛠️ Project Structure

```
├── src/
│   ├── main.c                 # Main game loop and state manager
│   ├── conrad.c / .h          # 357-frame Conrad rotoscoped animation engine & physics
│   ├── actors.c / .h          # Enemies, security drones, interactive items, bullets
│   ├── video_player.c / .h    # FMV cutscene streaming engine
│   ├── title_menu.c / .h      # Interactive title screen menu
│   ├── instructions.c / .h    # Controller guide and story instructions
│   ├── audio.c / .h           # SN76489 PSG sound effects engine
│   ├── hud.c / .h             # Shield battery & inventory HUD overlay
│   ├── sprite_patterns.c / .h # Fast VDP static sprite pattern loader
│   ├── cutscenes_data.c / .h  # Cutscene descriptors & frame tables
│   └── menu_font.h            # 8x8 bitmap font patterns
├── all_data_banks.bin         # Consolidated 79 16KB data banks (1264 KB)
├── game_banks.h               # Bank mapping definitions
├── mkrom_4mb.py               # 4MB Master System ROM builder with Sega header
├── generate_all_data_banks.py # Master data packaging script
├── generate_perfect_conrad_banks.py # Conrad 14-bank frame builder
├── process_rooms.py           # Titan Jungle screen and room converter
├── test_playtest.txt          # Automated regression test script for smstest
├── test_moveset.txt           # Player moveset verification test script
├── test_pickup.txt            # Floor item pickup verification test script
├── Makefile                   # SDCC / devkitSMS build configuration
└── Flashback_SMS.sms          # 4096 KB Sega Master System ROM
```

---

## ⚙️ Building the ROM

### Prerequisites
- [SDCC](https://sdcc.sourceforge.net/) (Small Device C Compiler) with Z80 support
- [devkitSMS](https://github.com/sverx/devkitSMS) (SMSlib, PSGlib, ihx2sms)
- Python 3 with Pillow and NumPy (`pip install Pillow numpy`)

### Build Commands
```bash
# Clean and compile the 4MB Sega Master System ROM
make clean
make

# Run automated verification with smstest
smstest Flashback_SMS.sms test_playtest.txt
smstest Flashback_SMS.sms test_moveset.txt
smstest Flashback_SMS.sms test_pickup.txt
```
