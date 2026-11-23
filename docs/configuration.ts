document.addEventListener('DOMContentLoaded', () => {
  const host = document.querySelector<HTMLInputElement>('#host-authority')
  if (!host) return

  for (const table of document.querySelectorAll<HTMLTableElement>('table.doxtable')) {
    let heading = table.previousElementSibling
    while (heading && heading.tagName !== 'H2') heading = heading.previousElementSibling

    const section = heading?.textContent?.trim().toLowerCase()
    const body = table.tBodies[0]
    if (!section || !body) continue

    const row = body.insertRow(0)
    const cell = row.insertCell()
    cell.colSpan = 3

    const link = document.createElement('code')
    link.className = 'open-button'
    link.textContent = 'Open'
    link.addEventListener('click', () => {
      let url: URL
      try {
        url = new URL('/config/', `https://${host.value.trim()}`)
      } catch {
        return
      }
      url.hash = section
      window.open(url, '_blank', 'noopener')
    })
    cell.appendChild(link)
  }
})

export {}
