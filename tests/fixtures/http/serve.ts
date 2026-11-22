const server = Bun.serve({
  port: 3000,
  hostname: '0.0.0.0',
  routes: {
    '/hello.txt': () => new Response(Bun.file(new URL('./hello.txt', import.meta.url))),
    '/hello-redirect.txt': () => new Response(Bun.file(new URL('./hello-redirect.txt', import.meta.url))),
  },
  fetch: () => new Response('Not found', { status: 404 }),
})

console.log(`Serving HTTP test fixtures on ${server.url}`)
