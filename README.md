# CoD1Plus - Player Stats Tracker for Call of Duty 1 v1.5

Simple and reliable hook system to collect and send player statistics to an HTTP backend.

## 📁 Structure

```
cod1plus/
├── src/
│   └── cod1plus.c          # Main hook code (simple, CodExtended-style)
├── scripts/
│   └── build.sh            # Build script
├── backend/
│   ├── server.js           # Node.js Express backend
│   └── package.json
├── build/
│   └── cod1plus.so         # Compiled library
└── archive/                # Old/unused code
```

## 🚀 Quick Start

### 1. Compile
```bash
cd scripts
bash build.sh
```

### 2. Start Backend
```bash
cd backend
PORT=3005 npm start
```

### 3. Run Server
```bash
LD_PRELOAD=./cod1plus.so ./cod_lnxded +set net_ip 0.0.0.0 +set dedicated 2 +exec server.cfg +map mp_harbor
```

### 4. Check Stats
```bash
curl http://localhost:3005/api/stats
```

## 📊 How it Works

- **No SV_Frame hook** (avoids crashes)
- **Direct memory reading** from `ADDR_SVS_CLIENTS`
- **Background thread** collects stats every 5 seconds
- **Simple HTTP POST** to backend

Based on CodExtended v1.5 approach for CoD1 Linux.

## 🔧 Configuration

Edit `src/cod1plus.c`:
```c
#define BACKEND_URL "http://localhost:3005"
#define STATS_ENDPOINT "/api/stats"
```

## ✅ Tested on

- CoD1 v1.5 Linux (cod_lnxded)
- Debian/Ubuntu Linux
- 32-bit compilation

## 📝 License

GPL-3.0
