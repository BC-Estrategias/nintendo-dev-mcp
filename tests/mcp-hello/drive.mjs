// Cliente roteirizado: fala com um servidor stdio usando o handshake EXATO capturado dos
// clientes reais (Claude Code 2.1.177 e Codex 0.134.0) e exercita tools/list + tools/call.
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';

const [, , serverPath, era] = process.argv;
const INIT = {
  claude: { protocolVersion: '2025-11-25', capabilities: { roots: {}, elicitation: {} },
            clientInfo: { name: 'claude-code', title: 'Claude Code', version: '2.1.177' } },
  codex:  { protocolVersion: '2025-06-18', capabilities: { elicitation: {} },
            clientInfo: { name: 'codex-mcp-client', title: 'Codex', version: '0.134.0' } },
}[era];
if (!INIT) throw new Error('era: claude|codex');

const child = spawn('node', [serverPath], { stdio: ['pipe', 'pipe', 'inherit'] });
const pending = new Map(); let nextId = 0;
createInterface({ input: child.stdout }).on('line', (l) => {
  let m; try { m = JSON.parse(l); } catch { console.log('  (non-JSON from server):', l.slice(0, 200)); return; }
  const p = pending.get(m.id); if (p) { pending.delete(m.id); p(m); }
});
const rpc = (method, params) => new Promise((res, rej) => {
  const id = nextId++; pending.set(id, res);
  child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
  setTimeout(() => rej(new Error(`timeout: ${method}`)), 8000);
});
const short = (o) => JSON.stringify(o).slice(0, 260);
try {
  const init = await rpc('initialize', INIT);
  console.log(`[${era}] initialize ->`, short(init.result ?? init.error));
  child.stdin.write(JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }) + '\n');
  const list = await rpc('tools/list', {});
  console.log(`[${era}] tools/list  ->`, short(list.result ?? list.error));
  const name = (list.result?.tools ?? [])[0]?.name;
  if (name) {
    const call = await rpc('tools/call', { name, arguments: { text: 'ola' } });
    console.log(`[${era}] tools/call  ->`, short(call.result ?? call.error));
  }
} catch (e) { console.log(`[${era}] FALHOU:`, e.message); }
child.kill();
