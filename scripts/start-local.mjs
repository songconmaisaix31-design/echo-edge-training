import {randomBytes} from 'node:crypto'
import {mkdir} from 'node:fs/promises'
import {join} from 'node:path'
import {createWeb, isolatedEnvironment, prepareDatabase, root, startApi, stopApi} from './workstation-services.mjs'

const directory = join(root, 'work', 'local-workstation')
const env = isolatedEnvironment(directory, 'platform.db')
const python = process.env.ECHO_LOCAL_PYTHON || (process.platform === 'win32' ? 'python' : 'python3')
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
  console.log(`Echo Edge 本机工作站 http://127.0.0.1:${port}/`)
} catch (error) {
  await stop()
  throw error
}
