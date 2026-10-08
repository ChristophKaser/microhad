import { defineConfig } from 'vite'

export default defineConfig({
  server: {
    proxy: {
      '/api': { target: 'http://192.168.178.62', changeOrigin: true },
    },
  },
})
