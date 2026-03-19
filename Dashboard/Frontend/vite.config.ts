import tailwindcss from '@tailwindcss/vite';
import { defineConfig } from 'vite';
import solidPlugin from 'vite-plugin-solid';
import devtools from 'solid-devtools/vite';

export default defineConfig({
  plugins: [devtools(), solidPlugin(), tailwindcss()],
  css: {
    transformer: "lightningcss",
    lightningcss: {
      // LightningCSS supports nesting, but Vite's Drafts type doesn't expose it yet.
      drafts: {
        nesting: true,
      } as any,
    },
  },
  server: {
    port: 3000,
  },
  build: {
    target: 'esnext',
  },
});
