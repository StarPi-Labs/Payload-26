import tailwindcss from '@tailwindcss/vite';
import { defineConfig } from 'vite';
import solidPlugin from 'vite-plugin-solid';

export default defineConfig({
  plugins: [solidPlugin(), tailwindcss()],
  server: {
    port: 5173,
    // `npm run dev` talks to `python lab_server.py` running next to it.
    proxy: { '/api': 'http://127.0.0.1:5050' },
  },
  build: {
    target: 'esnext',
    chunkSizeWarningLimit: 1500,
  },
});
