import {randomBytes} from 'node:crypto'
import {existsSync} from 'node:fs'
import {mkdir, readFile, stat} from 'node:fs/promises'
import {isAbsolute, join} from 'node:path'
import {parseEnv} from 'node:util'
import {createWeb, isolatedEnvironment, prepareDatabase, root, startApi, stopApi} from './workstation-services.mjs'

const directory = join(root, 'work', 'local-workstation')
const env = isolatedEnvironment(directory, 'platform.db')
const settingsFile = join(directory, 'settings.env')
const settings = existsSync(settingsFile) ? parseEnv(await readFile(settingsFile, 'utf8')) : {}
const modelFiles = [
  'ECHO_INFERENCE_PYTHON', 'ECHO_INFERENCE_SCRIPT', 'ECHO_ENGINE_REPORT',
  'ECHO_STANFORD_CHECKPOINT', 'ECHO_STRUCTURE_PYTHON', 'ECHO_CARDIACUDC_CHECKPOINT',
  'ECHO_STEPFUN_KEY_FILE', 'ECHO_STEPFUN_SAMPLES_FILE',
]
for (const name of modelFiles) {
  const path = process.env[name] ?? settings[name]
  if (!path) continue
  if (!isAbsolute(path) || !(await stat(path)).isFile()) throw new Error(`${name} 须为已有文件的绝对路径。`)
  env[name] = path
}
const python = process.env.ECHO_LOCAL_PYTHON || settings.ECHO_LOCAL_PYTHON || (process.platform === 'win32' ? 'python' : 'python3')
const port = Number(process.env.ECHO_LOCAL_WEB_PORT ?? 5176)
if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error('本机工作站端口无效。')
const operator = {name: 'local-operator', password: randomBytes(24).toString('base64url')}

let api
let web
async function stop() {
  try { await web?.close() }
  finally { await stopApi(api) }
}

for (const signal of ['SIGINT', 'SIGTERM']) process.once(signal, () => {
  void stop().catch(error => { console.error(error); process.exitCode = 1 })
})

try {
  await mkdir(directory, {recursive: true})
  await prepareDatabase(python, env, operator)
  const service = startApi(python, env, data => process.stdout.write(data))
  api = service.child
  web = await createWeb({target: await service.ready, port, cacheDir: join(directory, 'vite'), operator})
  await web.listen()
  console.log(`Cardio bridge 本机工作站 http://127.0.0.1:${port}/`)
} catch (error) {
  await stop()
  throw error
}
