# K10 Anime Customer Service Avatar Assets

Aset animasi anime 8 pose untuk display DFRobot K10 (240×320 LCD), diproses langsung dari master sprite sheet 2×4 (`assets/Eight-Panel Anime Customer Service Sprite Sheet.png`).

---

## 1. File Aset Master 8 Pose (`assets/k10_anime/`)

Semua 8 file mempertahankan **100% ekspresi asli, mata, mulut, dan gestur** dari gambar master tanpa overlay buatan:

| File | Pose / Ekspresi | Sumber Grid 2×4 |
| :--- | :--- | :--- |
| `idle.png` | Wajah ramah tersenyum, mata terbuka menatap ke depan | Baris 1, Kolom 1 |
| `blink.png` | Kedua mata tertutup rapat melengkung alami (kedipan penuh) | Baris 1, Kolom 2 |
| `listening.png` | Tangan kanan menyentuh headset cup, ekspresi senyum | Baris 1, Kolom 3 |
| `thinking.png` | Jari telunjuk di dagu, mata menatap ke atas-kanan | Baris 1, Kolom 4 |
| `speaking_1.png` | Mulut terbuka kecil (fonem vokal sedang / 'O') | Baris 2, Kolom 1 |
| `speaking_2.png` | Mulut terbuka lebar (fonem bicara penuh / tertawa) | Baris 2, Kolom 2 |
| `happy.png` | Kedua mata melengkung bahagia (`^ ^`), mulut tersenyum lebar | Baris 2, Kolom 3 |
| `wink.png` | Mengedipkan mata kiri (*playful wink*), mata kanan terbuka | Baris 2, Kolom 4 |

---

## 2. Alinyemen Canvas & Preservasi Proporsi

- **Dimensi Canvas**: `384 × 512` piksel (Format RGBA 32-bit).
- **Rasio Aspek 3:4**: Tepat sama dengan rasio LCD DFRobot K10 (`240 × 320`). Skala konversi ke layar K10 adalah `1.6×` tanpa distorsi, tanpa gepeng, dan tanpa molor (*zero stretch*).
- **Piksel Asli 1:1**: Resolusi gambar asli karakter dipertahankan seutuhnya (*unscaled master*).
- **Alinyemen Kepala & Headset Terkunci**:
  - Puncak bando headset (*apex*): tepat di $Y = 18$ di seluruh 8 pose.
  - Pusat earpiece headset kiri: tepat di $(X = 102.8, Y = 153.2)$ di seluruh 8 pose.
  - Posisi kepala dan headset terkunci simetris sehingga saat berganti pose, kepala tidak meloncat (*no jump/jitter*).

---

## 3. Pembersihan Artefak & Background

- **Metode**: BFS Connected-Component Segmentation pada piksel karakter utama.
- **Hasil**:
  - Garis batas kotak sprite sheet di $X = 325..328$ dan $Y = 555..659$ dibersihkan tuntas.
  - Halo semi-transparan dan *compression noise* (`alpha <= 15`) dihapus bersih.
  - Background 100% transparan (`RGBA = 0, 0, 0, 0`).
  - Tidak ada overlay atau penempelan mata/mulut yang merusak ekspresi asli.

---

## 4. Preview Animasi GIF

Tersedia 3 file animasi preview yang telah diverifikasi secara visual:
1. `preview_idle_blink.gif`:
   - Siklus santai dengan jeda mata terbuka alami (~2.2 detik) lalu kedipan cepat kedua mata tertutup (~150 ms) dan kembali membuka.
2. `preview_speaking_lipsync.gif`:
   - Tiga tingkat bukaan mulut terlihat nyata: **Level 1 (Tertutup)** $\rightarrow$ **Level 2 (Kecil)** $\rightarrow$ **Level 3 (Besar)** $\rightarrow$ **Level 2** $\rightarrow$ **Level 1**.
3. `preview_all_poses.gif`:
   - Siklus berputar dari seluruh 8 pose asli (`idle` $\rightarrow$ `blink` $\rightarrow$ `listening` $\rightarrow$ `thinking` $\rightarrow$ `speaking_1` $\rightarrow$ `speaking_2` $\rightarrow$ `happy` $\rightarrow$ `wink`).

---

## 5. Script Otomatisasi

Script pemrosesan tersimpan di:
`scripts/prepare_k10_anime.py`
Jalankan kapan saja via:
```bash
python scripts/prepare_k10_anime.py
```
