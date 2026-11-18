import {defineConfig} from 'vite'
import tailwind from '@tailwindcss/vite'

export default defineConfig({
  plugins: [tailwind()],
  define: {
    __ECHO_LOCAL_WORKSTATION__: 'false',
    __ECHO_PUBLIC_SAMPLES__: JSON.stringify(process.env.ECHO_PUBLIC_SAMPLES === '1'),
  },
  server: {
    host: '127.0.0.1',
    port: 5176,
    strictPort: true,
    proxy: {'/api': process.env.ECHO_API_TARGET || 'http://127.0.0.1:8004'},
  },
})
