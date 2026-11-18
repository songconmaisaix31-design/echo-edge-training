const sessionPath = '/api/v1/local-session'

function reply(response, status, value) {
  response.statusCode = status
  response.setHeader('Content-Type', 'application/json; charset=utf-8')
  response.setHeader('Cache-Control', 'no-store')
  response.end(JSON.stringify(value))
}

export function localSessionPlugin({target, name, password}) {
  const upstream = new URL(target)
  if (upstream.protocol !== 'http:' || upstream.hostname !== '127.0.0.1' || !upstream.port || upstream.username || upstream.password) {
    throw new Error('本机工作站的后端须使用指定端口的 IPv4 回环地址。')
  }
  const requestApi = (path, init) => fetch(new URL(path, upstream), {
    ...init, redirect: 'error', signal: AbortSignal.timeout(5000),
  })
  const isLocalOperator = user => user?.name === name && user?.role === 'operator'

  return {
    name: 'echo-local-session',
    apply: 'serve',
    configureServer(server) {
      if (server.config.server.host !== '127.0.0.1') throw new Error('本机工作站须监听 127.0.0.1。')
      server.middlewares.use(async (request, response, next) => {
        if (request.url !== sessionPath) return next()
        const host = `127.0.0.1:${request.socket.localPort}`
        if (request.socket.remoteAddress !== '127.0.0.1' || request.headers.host !== host || request.headers.origin !== `http://${host}`) {
          return reply(response, 403, {detail: '仅允许从本机工作站连接。'})
        }
        if (request.method !== 'POST') {
          response.setHeader('Allow', 'POST')
          return reply(response, 405, {detail: '请求方法无效。'})
        }

        try {
          if (request.headers.cookie) {
            const current = await requestApi('/api/v1/me', {headers: {cookie: request.headers.cookie}})
            if (!current.ok && current.status !== 401) throw new Error('会话检查失败。')
            if (current.ok && isLocalOperator(await current.json())) {
              return reply(response, 200, {name, role: 'operator'})
            }
          }
          const login = await requestApi('/api/v1/login', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({name, password}),
          })
          if (!login.ok || !isLocalOperator(await login.json())) throw new Error('本机操作员会话建立失败。')
          const [cookie] = login.headers.getSetCookie()
          if (!cookie) throw new Error('后端未发放会话。')
          response.setHeader('Set-Cookie', cookie)
          reply(response, 200, {name, role: 'operator'})
        } catch {
          reply(response, 502, {detail: '无法连接本机工作站，请确认后端正在运行。'})
        }
      })
    },
  }
}
