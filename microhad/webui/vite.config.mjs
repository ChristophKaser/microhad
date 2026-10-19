import { execFileSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'
import { defineConfig } from 'vite'

export default defineConfig(({ command }) => {
  // Development-only host settings. Never load them into the browser bundle.
  if (command !== 'serve') return {}
  const loader = fileURLToPath(new URL('../../scripts/workspace-env.sh', import.meta.url))
  const gateway = execFileSync('sh', [loader, '--get', 'GATEWAY_IP'], { encoding: 'utf8' }).trim()
  if (!gateway) throw new Error('Set GATEWAY_IP in workspace.local.env or the environment for the development proxy')
  return {
    server: {
      proxy: {
        '/api': { target: `http://${gateway}`, changeOrigin: true },
      },
    },
  }
})
