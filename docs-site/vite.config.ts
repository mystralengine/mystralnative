import { defineConfig, type Plugin } from 'vite';
import react from '@vitejs/plugin-react';
import mdx from '@mdx-js/rollup';
import remarkGfm from 'remark-gfm';
import rehypeHighlight from 'rehype-highlight';

// Wrap the MDX plugin to skip ?raw imports so Vite's built-in
// raw handler returns the file content as a string (not a component).
const mdxPlugin = mdx({
  remarkPlugins: [remarkGfm],
  rehypePlugins: [rehypeHighlight],
}) as Plugin;

const origTransform = mdxPlugin.transform as Function;
mdxPlugin.transform = function (this: unknown, code: string, id: string, ...args: unknown[]) {
  if (id.includes('?')) return undefined;
  return origTransform.call(this, code, id, ...args);
};

export default defineConfig({
  base: '/mystralnative/',
  plugins: [mdxPlugin, react()],
  build: {
    outDir: 'dist',
  },
});
