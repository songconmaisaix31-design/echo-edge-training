import {spawn} from 'node:child_process'
import {once} from 'node:events'
import {createRequire} from 'node:module'
import {join} from 'node:path'
import {fileURLToPath, pathToFileURL} from 'node:url'
import {localSessionPlugin} from './local-session.mjs'

export const root = fileURLToPath(new URL('../', import.meta.url))
export const frontend = join(root, 'frontend')
const backend = join(root, 'backend')
export const requireFrontend = createRequire(join(frontend, 'package.json'))

export function isolatedEnvironment(directory, database) {
  const variables = Object.entries(process.env).filter(([name]) => !/^ECHO_|^PYTHONPATH$/i.test(name))
  return {
    ...Object.fromEntries(variables),
    ECHO_DATABASE_URL: `sqlite:///${join(directory, database).replaceAll('\\', '/')}`,
    ECHO_MEDIA_ROOT: join(directory, 'media'),
    PYTHONUTF8: '1',
    TMP: directory,
    TEMP: directory,
    TMPDIR: directory,
  }
}

export async function runPython(python, env, args) {
  const child = spawn(python, args, {cwd: backend, env, stdio: 'inherit', windowsHide: true})
  const [code] = await once(child, 'exit')
  if (code !== 0) throw new Error(`Python 命令执行失败（${code}） ${args.join(' ')}`)
}

export async function prepareDatabase(python, env, {name, password}) {
  await runPython(python, env, ['-m', 'alembic', 'upgrade', 'head'])
  await runPython(python, {...env, ECHO_OPERATOR_NAME: name, ECHO_OPERATOR_PASSWORD: password}, [join(root, 'scripts', 'ensure-operator.py')])
}

export function startApi(python, env, onOutput) {
  const child = spawn(python, ['-m', 'uvicorn', 'main:app', '--host', '127.0.0.1', '--port', '0', '--no-use-colors'], {
    cwd: backend, env, stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true,
  })
  child.stdout.on('data', onOutput)
  const ready = new Promise((resolve, reject) => {
    let startupOutput = ''
    const timeout = setTimeout(() => reject(new Error('后端 30 秒内未启动')), 30_000)
    child.once('error', error => { clearTimeout(timeout); reject(error) })
    child.once('exit', code => { clearTimeout(timeout); reject(new Error(`后端退出（${code}）`)) })
    child.stderr.on('data', data => {
      onOutput(data)
      if (startupOutput === null) return
      startupOutput += data
      const match = startupOutput.match(/Uvicorn running on (http:\/\/127\.0\.0\.1:\d+)/)
      if (match) {
        clearTimeout(timeout)
        startupOutput = null
        resolve(match[1])
      }
    })
  })
  return {child, ready}
}

export async function stopApi(child) {
  if (!child?.pid || child.exitCode !== null || child.signalCode !== null) return
  const exited = once(child, 'exit')
  child.kill()
  await exited
}

export async function createWeb({target, port, cacheDir, operator}) {
  const {createServer} = await import(pathToFileURL(requireFrontend.resolve('vite')).href)
  return createServer({
    root: frontend,
    cacheDir,
    ...(operator ? {
      plugins: [localSessionPlugin({target, ...operator})],
      define: {__ECHO_LOCAL_WORKSTATION__: 'true'},
    } : {}),
    server: {host: '127.0.0.1', port, strictPort: true, proxy: {'/api': target}},
  })
}
