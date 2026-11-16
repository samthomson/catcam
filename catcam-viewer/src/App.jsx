import React, { useEffect, useRef, useState } from 'react'

const RELAY = 'wss://relay-catcam.samt.st'
const AUTHOR = '23168823f2f310372b8a45810608a8947802dd956c07213bc43c6d6b81d64289'
const REQ_LIMIT = 200
const QUIET_RECONNECT_MS = 45000 // if the relay goes silent this long, silently resubscribe

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

function caption(ev) {
  const signal = tag(ev, 'signal')
  const t = `${new Date(ev.created_at * 1000).toLocaleString()} · ${timeAgo(ev.created_at)}`
  return signal ? `${t} · ${signal}` : t
}

export default function App() {
  const [events, setEvents] = useState([])
  const [status, setStatus] = useState('connecting…')
  const [lightboxId, setLightboxId] = useState(null) // event id
  const [infoEvent, setInfoEvent] = useState(null) // raw nostr event

  const wsRef = useRef(null)
  const subRef = useRef(0)
  const lastMsgRef = useRef(Date.now())
  const intentionalRef = useRef(false)

  useEffect(() => {
    let disposed = false
    let retryTimer, quietTimer

    const subscribe = (ws) => {
      subRef.current += 1
      ws.send(JSON.stringify(['REQ', `cam-${subRef.current}`, { authors: [AUTHOR], kinds: [1], limit: REQ_LIMIT }]))
    }

    const connect = () => {
      if (disposed) return
      setStatus('connecting…')
      const ws = new WebSocket(RELAY)
      wsRef.current = ws

      ws.onopen = () => {
        subscribe(ws)
        setStatus('live')
      }

      ws.onmessage = (m) => {
        lastMsgRef.current = Date.now()
        const msg = JSON.parse(m.data)
        if (msg[0] === 'EVENT') {
          const ev = msg[2]
          setEvents((prev) =>
            prev.some((e) => e.id === ev.id) ? prev : [...prev, ev].sort((a, b) => b.created_at - a.created_at),
          )
        } else if (msg[0] === 'CLOSED') {
          subscribe(ws) // relay ended the subscription (e.g. idle timeout) — open a fresh one
        } else if (msg[0] === 'NOTICE') {
          setStatus(`notice: ${msg[1]}`)
        }
      }

      ws.onclose = () => {
        if (disposed) return
        setStatus(intentionalRef.current ? 'live' : 'reconnecting…')
        retryTimer = setTimeout(connect, intentionalRef.current ? 150 : 3000)
        intentionalRef.current = false
      }
    }

    // watchdog: a silently-dead socket (or an idle-closed subscription) recovers without a flicker
    quietTimer = setInterval(() => {
      if (!disposed && Date.now() - lastMsgRef.current > QUIET_RECONNECT_MS) {
        intentionalRef.current = true
        wsRef.current?.close()
      }
    }, 15000)

    connect()
    return () => {
      disposed = true
      clearTimeout(retryTimer)
      clearInterval(quietTimer)
      wsRef.current?.close()
    }
  }, [])

  const lbIndex = lightboxId === null ? -1 : events.findIndex((e) => e.id === lightboxId)
  const lbEvent = lbIndex >= 0 ? events[lbIndex] : null

  // Esc closes overlays; arrows navigate the lightbox
  useEffect(() => {
    const onKey = (e) => {
      if (e.key === 'Escape') {
        setLightboxId(null)
        setInfoEvent(null)
      }
      if (lbEvent && e.key === 'ArrowLeft' && lbIndex > 0) setLightboxId(events[lbIndex - 1].id)
      if (lbEvent && e.key === 'ArrowRight' && lbIndex < events.length - 1) setLightboxId(events[lbIndex + 1].id)
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [lbEvent, lbIndex, events])

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
                <img src={url} alt="" loading="lazy" onClick={() => setLightboxId(ev.id)} />
                <figcaption>
                  <span className="when">
                    {new Date(ev.created_at * 1000).toLocaleString()} · {timeAgo(ev.created_at)}
                  </span>
                  <span className="meta">
                    {signal && <span className="signal">{signal}</span>}
                    <button className="info" title="raw event" onClick={() => setInfoEvent(ev)}>
                      i
                    </button>
                  </span>
                </figcaption>
              </figure>
            )
          })}
        </main>
      )}

      {lbEvent && (
        <div className="overlay" onClick={() => setLightboxId(null)}>
          <div className="lb" onClick={(e) => e.stopPropagation()}>
            <img className="big" src={imageUrl(lbEvent)} alt="" />
            <div className="lb-cap">
              <span>{caption(lbEvent)}</span>
              <button className="info" title="raw event" onClick={() => setInfoEvent(lbEvent)}>
                i
              </button>
            </div>
          </div>
          <button
            className="nav prev"
            disabled={lbIndex <= 0}
            onClick={() => lbIndex > 0 && setLightboxId(events[lbIndex - 1].id)}
          >
            ‹
          </button>
          <button
            className="nav next"
            disabled={lbIndex >= events.length - 1}
            onClick={() => lbIndex < events.length - 1 && setLightboxId(events[lbIndex + 1].id)}
          >
            ›
          </button>
        </div>
      )}

      {infoEvent && (
        <div className="overlay" onClick={() => setInfoEvent(null)}>
          <pre className="json" onClick={(e) => e.stopPropagation()}>
            {JSON.stringify(infoEvent, null, 2)}
          </pre>
        </div>
      )}
    </div>
  )
}
