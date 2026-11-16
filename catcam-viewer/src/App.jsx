import React, { useEffect, useRef, useState } from 'react'

const RELAY = 'wss://relay-catcam.samt.st'
const AUTHOR = '23168823f2f310372b8a45810608a8947802dd956c07213bc43c6d6b81d64289'

function tag(event, name) {
  const t = event.tags?.find((t) => t[0] === name)
  return t ? t[1] : null
}

function imageUrl(event) {
  const imeta = event.tags?.find((t) => t[0] === 'imeta')
  if (imeta) {
    const url = imeta.find((v) => v.startsWith('url '))
    if (url) return url.slice(4)
  }
  return event.content // firmware puts the bare URL in content
}

function timeAgo(ts) {
  const s = Math.floor(Date.now() / 1000 - ts)
  if (s < 60) return `${s}s ago`
  if (s < 3600) return `${Math.floor(s / 60)}m ago`
  if (s < 86400) return `${Math.floor(s / 3600)}h ago`
  return `${Math.floor(s / 86400)}d ago`
}

export default function App() {
  const [events, setEvents] = useState([])
  const [status, setStatus] = useState('connecting…')
  const sockRef = useRef(null)

  useEffect(() => {
    let closed = false
    let retry

    const connect = () => {
      if (closed) return
      setStatus('connecting…')
      const ws = new WebSocket(RELAY)
      sockRef.current = ws

      ws.onopen = () => {
        ws.send(JSON.stringify(['REQ', 'cam', { authors: [AUTHOR], kinds: [1], limit: 200 }]))
        setStatus('live')
      }

      ws.onmessage = (m) => {
        const msg = JSON.parse(m.data)
        if (msg[0] === 'EVENT' && msg[1] === 'cam') {
          const ev = msg[2]
          setEvents((prev) =>
            prev.some((e) => e.id === ev.id) ? prev : [...prev, ev].sort((a, b) => b.created_at - a.created_at),
          )
        } else if (msg[0] === 'NOTICE') {
          setStatus(`notice: ${msg[1]}`)
        }
      }

      ws.onclose = () => {
        if (closed) return
        setStatus('disconnected — retrying')
        retry = setTimeout(connect, 3000)
      }
    }

    connect()
    return () => {
      closed = true
      clearTimeout(retry)
      sockRef.current?.close()
    }
  }, [])

  return (
    <div className="app">
      <header>
        <h1>🐱 Cat Cam</h1>
        <span className={`status ${status === 'live' ? 'ok' : ''}`}>{status}</span>
        <span className="count">{events.length} photos</span>
      </header>

      {events.length === 0 ? (
        <p className="empty">
          {status === 'live' ? 'connected — waiting for events… wave at the cat cam' : 'no events yet'}
        </p>
      ) : (
        <main>
          {events.map((ev) => {
            const url = imageUrl(ev)
            const signal = tag(ev, 'signal')
            return (
              <figure key={ev.id} className="card">
                <a href={url} target="_blank" rel="noreferrer">
                  <img src={url} alt="" loading="lazy" />
                </a>
                <figcaption>
                  <time dateTime={new Date(ev.created_at * 1000).toISOString()}>
                    {new Date(ev.created_at * 1000).toLocaleString()} · {timeAgo(ev.created_at)}
                  </time>
                  {signal && <span className="signal">{signal}</span>}
                </figcaption>
              </figure>
            )
          })}
        </main>
      )}
    </div>
  )
}
