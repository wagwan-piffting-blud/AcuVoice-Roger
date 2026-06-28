// server.js - tiny static file server with HTTP Range support (mirrors GitHub Pages/Fastly),
// so the browser test exercises the real partial-content path. Usage: node server.js [port] [root]
const http = require('http'), fs = require('fs'), path = require('path');
const ROOT = path.resolve(process.argv[3] || path.join(__dirname, '..', 'site'));
const PORT = parseInt(process.argv[2] || '8753', 10);
const MIME = { '.html':'text/html', '.js':'text/javascript', '.wasm':'application/wasm',
  '.data':'application/octet-stream', '.ply':'application/octet-stream', '.cmp':'application/octet-stream',
  '.fle':'application/octet-stream', '.tab':'application/octet-stream', '.dvl':'application/octet-stream',
  '.srt':'application/octet-stream', '.json':'application/json', '.css':'text/css', '.wav':'audio/wav' };

const server = http.createServer((req, res) => {
  let p = decodeURIComponent(req.url.split('?')[0]);
  if (p === '/') p = '/index.html';
  const file = path.join(ROOT, p);
  if (!file.startsWith(ROOT) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
    res.writeHead(404); res.end('not found'); return;
  }
  const size = fs.statSync(file).size;
  const type = MIME[path.extname(file).toLowerCase()] || 'application/octet-stream';
  const range = req.headers.range;
  res.setHeader('Accept-Ranges', 'bytes');
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  if (range) {
    const m = /bytes=(\d+)-(\d*)/.exec(range);
    const start = parseInt(m[1], 10);
    const end = m[2] ? parseInt(m[2], 10) : size - 1;
    res.writeHead(206, { 'Content-Type': type, 'Content-Range': `bytes ${start}-${end}/${size}`, 'Content-Length': end - start + 1 });
    fs.createReadStream(file, { start, end }).pipe(res);
  } else {
    res.writeHead(200, { 'Content-Type': type, 'Content-Length': size });
    fs.createReadStream(file).pipe(res);
  }
});
server.listen(PORT, () => console.error(`[server] http://localhost:${PORT}  root=${ROOT}`));
module.exports = { server, PORT };
