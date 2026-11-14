import {defineConfig} from 'vite'
import tailwind from '@tailwindcss/vite'
export default defineConfig({plugins:[tailwind()],server:{host:'127.0.0.1',port:5176,strictPort:true,proxy:{'/api':'http://127.0.0.1:8004'}}})





