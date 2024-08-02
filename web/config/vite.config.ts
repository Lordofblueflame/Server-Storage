import { defineConfig } from 'vite';
import path from 'path';

export default defineConfig({
    build: {
        outDir: 'dist',
        rollupOptions: {
            input: {
                main: path.resolve(__dirname, 'web/src/index.ts')
            }
        }
    },
    resolve: {
        alias: {
            '@': path.resolve(__dirname, 'web/src')
        }
    }
});
