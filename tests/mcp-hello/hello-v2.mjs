// Servidor hello com o SDK MCP v2 (@modelcontextprotocol/server) sobre stdio.
// A factory registra as tools uma vez; serveStdio atende tanto clientes da spec 2026-07-28
// quanto clientes da era `initialize` (2025-06-18 / 2025-11-25).
import { McpServer } from '@modelcontextprotocol/server';
import { serveStdio } from '@modelcontextprotocol/server/stdio';
import * as z from 'zod/v4';

serveStdio(() => {
  const server = new McpServer({ name: 'hello-v2', version: '0.0.1' }, { capabilities: { tools: {} } });
  server.registerTool(
    'hello',
    { description: 'Hello tool. Echoes the given text.', inputSchema: z.object({ text: z.string() }) },
    async ({ text }) => ({ content: [{ type: 'text', text: `hello: ${text}` }] }),
  );
  return server;
});
