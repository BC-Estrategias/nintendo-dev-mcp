// Sonda MCP: servidor stdio mínimo que REGISTRA toda mensagem recebida, para descobrir
// que revisão do protocolo o cliente fala. Não depende de SDK.
import { createInterface } from 'node:readline';
import { appendFileSync } from 'node:fs';

const LOG = process.env.PROBE_LOG || '/tmp/mcp-probe.log';
const log = (dir, obj) => appendFileSync(LOG, `${dir} ${typeof obj === 'string' ? obj : JSON.stringify(obj)}\n`);
const send = (obj) => { log('>>', obj); process.stdout.write(JSON.stringify(obj) + '\n'); };

const TOOL = {
  name: 'ping',
  description: 'Probe tool. Returns "pong". Use it to confirm the MCP connection works.',
  inputSchema: { type: 'object', additionalProperties: false },
  annotations: { readOnlyHint: true },
};
const NEW_ERA = '2026-07-28';

createInterface({ input: process.stdin }).on('line', (line) => {
  if (!line.trim()) return;
  log('<<', line);
  let m; try { m = JSON.parse(line); } catch { return; }
  const { id, method, params } = m;
  if (id === undefined) return; // notificação
  const metaVer = params?._meta?.['io.modelcontextprotocol/protocolVersion'];
  const era = metaVer ? { resultType: 'complete' } : {};
  switch (method) {
    case 'initialize':
      return send({ jsonrpc: '2.0', id, result: {
        protocolVersion: params?.protocolVersion ?? '2025-06-18',
        capabilities: { tools: {} }, serverInfo: { name: 'probe', version: '0.0.1' } } });
    case 'tools/list':
      return send({ jsonrpc: '2.0', id, result: { ...era, tools: [TOOL] } });
    case 'tools/call':
      return send({ jsonrpc: '2.0', id, result: { ...era, content: [{ type: 'text', text: `pong (client protocol: ${metaVer ?? 'initialize-era'})` }], isError: false } });
    case 'ping':
      return send({ jsonrpc: '2.0', id, result: {} });
    default:
      return send({ jsonrpc: '2.0', id, error: { code: -32601, message: `Method not found: ${method}` } });
  }
});
