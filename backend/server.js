const express = require("express");
const fs = require("fs/promises");
const path = require("path");

const app = express();
app.use(express.json({ limit: "1mb" }));

const dataPath = path.join(__dirname, "stats.json");

async function readData() {
  try {
    const raw = await fs.readFile(dataPath, "utf8");
    return JSON.parse(raw);
  } catch (err) {
    if (err && err.code === "ENOENT") return [];
    throw err;
  }
}

async function writeData(data) {
  await fs.writeFile(dataPath, JSON.stringify(data, null, 2));
}

app.post("/api/stats", async (req, res) => {
  try {
    const entry = {
      received_at: new Date().toISOString(),
      payload: req.body
    };
    const data = await readData();
    data.push(entry);
    await writeData(data);
    res.json({ ok: true });
  } catch (err) {
    res.status(500).json({ ok: false });
  }
});

app.get("/api/stats", async (req, res) => {
  try {
    const data = await readData();
    res.json(data);
  } catch (err) {
    res.status(500).json({ ok: false });
  }
});

const port = Number(process.env.PORT || 3000);
app.listen(port, () => {
  process.stdout.write(`listening:${port}\n`);
});
