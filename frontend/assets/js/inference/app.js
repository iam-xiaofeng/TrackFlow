import { $, fmtTime, clamp } from '../shared/dom.js'
import { createLogger } from '../shared/log.js'
import { perspectiveTransform, undistortPoint, groundToLatLon } from '../shared/geo.js'
import { downloadCsv, revokeAllUrls, revokeUrl } from '../shared/download.js'

const vid = $('vid')
const cvs = $('cvs')
const cx = cvs.getContext('2d')
const cap = document.createElement('canvas')
const capCx = cap.getContext('2d')
const logger = createLogger('logB', 'logE')
const log = logger.log

let ws = null
let reconnectTimer = null
let streaming = false
let paused = false
let frameId = 0
let uploadScale = 1
let targetFps = 10
let videoLoaded = false
const CAPTURE_MAX_WIDTH = 1024
const MAX_INFLIGHT = 6
const inflightFrames = new Map()
const expiredRequests = new Set()
let nextFrameTime = 0
let seekBusy = false
let fpsT0 = 0
let fpsCnt = 0
let sourceVideoUrl = null
let targetTransform = { scale: 1, panX: 0, panY: 0 }
let laneDetectionEnabled = false
let trajLength = 100
let homographyMatrix = null
let originLon = 0
let originLat = 0
let cameraMatrix = null
let distCoeffs = null
let undistortEnabled = false
let selectedVehicle = null
let drawingLane = null
let laneCounter = 0
const vehicleTracks = {}
const lanes = {}
const labelWidthCache = new Map()
const laneColors = ['#ff8000','#00bfff','#ff4081','#76ff03','#e040fb','#ffea00','#00e5ff','#ff6e40']
const colors = ['#48f08b','#f28b82','#8ab4f8','#fdd663','#c58af9','#ff8bcb','#36d1a0','#78d9ec','#fcb75e','#a8c7fa']

const PING_INTERVAL = 30000
const INFLIGHT_TIMEOUT = 15000
let pingTimer = null
let inflightTimer = null
let manualDisconnect = false
let waitingForResetAck = false

function updatePlayIcon() {
  $('ppIco').innerHTML = vid.paused
    ? '<path d="M8 5v14l11-7z"></path>'
    : '<path d="M6 4h4v16H6zM14 4h4v16h-4z"></path>'
}

function setConn(on) {
  $('cd').classList.toggle('on', on)
  $('cl').textContent = on ? 'Connected' : 'Disconnected'
  $('cb').textContent = on ? 'Disconnect' : 'Connect'
  $('btnInfer').disabled = !on || !videoLoaded
}

function applyTransform() {
  const target = cvs.style.display !== 'none' ? cvs : vid
  target.style.transform = `translate(${targetTransform.panX}px,${targetTransform.panY}px) scale(${targetTransform.scale})`
}

function resetTransform() {
  targetTransform = { scale: 1, panX: 0, panY: 0 }
  applyTransform()
}

function toggleConn() {
  if (ws && ws.readyState === WebSocket.OPEN) {
    manualDisconnect = true
    clearTimeout(reconnectTimer)
    ws.close()
  } else {
    manualDisconnect = false
    connect(false)
  }
}

function scheduleReconnect() {
  if (manualDisconnect) return
  clearTimeout(reconnectTimer)
  reconnectTimer = setTimeout(() => connect(true), 2000)
}

function startPing() {
  stopPing()
  pingTimer = setInterval(() => {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'ping', request_id: 'keepalive' }))
    }
  }, PING_INTERVAL)
  inflightTimer = setInterval(() => {
    const now = performance.now()
    for (const [rid, entry] of inflightFrames) {
      if (now - entry.t0 > INFLIGHT_TIMEOUT) {
        expiredRequests.add(rid)
        inflightFrames.delete(rid)
        log(`Request ${rid} timed out, releasing slot`, 'error')
        pumpFrames()
      }
    }
  }, 3000)
}

function stopPing() {
  clearInterval(pingTimer)
  clearInterval(inflightTimer)
  pingTimer = null
  inflightTimer = null
}

function connect(isReconnect) {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws'
  const url = `${proto}://${location.host}/ws`
  if (!isReconnect) log(`Connecting to ${url}...`)
  ws = new WebSocket(url)
  ws.onopen = () => {
    manualDisconnect = false
    if (isReconnect) log('Reconnected', 'success')
    else log('Connected', 'success')
    setConn(true)
    startPing()
  }
  ws.onmessage = (event) => onMsg(event.data)
  ws.onerror = () => {
    log('Connection error', 'error')
    setConn(false)
  }
  ws.onclose = () => {
    stopPing()
    setConn(false)
    log('Disconnected', 'warn')
    if (streaming) stopInfer(false)
    scheduleReconnect()
  }
}

function revokeSourceVideo() {
  if (sourceVideoUrl) {
    revokeUrl(sourceVideoUrl)
    sourceVideoUrl = null
  }
}

function loadVideo(file) {
  if (!file) return
  revokeSourceVideo()
  sourceVideoUrl = URL.createObjectURL(file)
  vid.src = sourceVideoUrl
  vid.style.display = 'block'
  cvs.style.display = 'none'
  $('ph').style.display = 'none'
  $('tl').classList.remove('hidden')
  vid.onloadedmetadata = () => {
    cap.width = Math.min(vid.videoWidth, CAPTURE_MAX_WIDTH)
    const captureScale = cap.width / vid.videoWidth
    cap.height = Math.round(vid.videoHeight * captureScale)
    uploadScale = vid.videoWidth / cap.width
    cvs.width = vid.videoWidth
    cvs.height = vid.videoHeight
    videoLoaded = true
    setConn(ws && ws.readyState === WebSocket.OPEN)
    resetTransform()
    log(`Loaded: ${file.name} (${vid.videoWidth}x${vid.videoHeight}, ${fmtTime(vid.duration)})`, 'success')
  }
}

function buildRequestHeader() {
  const config = {}
  if (homographyMatrix) {
    config.geo_transform = {
      homography: Array.from(homographyMatrix),
      origin_lon: originLon,
      origin_lat: originLat,
    }
    if (cameraMatrix && distCoeffs) {
      config.geo_transform.camera_matrix = Array.from(cameraMatrix)
      config.geo_transform.dist_coeffs = Array.from(distCoeffs)
    }
  }
  if (cameraMatrix && distCoeffs) {
    config.undistort = {
      camera_matrix: Array.from(cameraMatrix),
      dist_coeffs: Array.from(distCoeffs),
    }
  }
  config.yolo = { confidence: 0.5, nms_threshold: 0.45 }

  return {
    type: 'infer_header',
    request_id: '',
    frame_id: 0,
    features: {
      tracker: true,
      undistort: undistortEnabled,
      geo_transform: Boolean(homographyMatrix && (originLon || originLat)),
    },
    config,
  }
}

function startInfer() {
  if (!ws || ws.readyState !== WebSocket.OPEN || !videoLoaded) return
  streaming = true
  paused = false
  waitingForResetAck = true
  frameId = 0
  inflightFrames.clear()
  expiredRequests.clear()
  targetFps = +$('fpsSel').value
  nextFrameTime = 0
  seekBusy = false
  fpsT0 = performance.now()
  fpsCnt = 0
  vid.pause()
  vid.currentTime = 0
  updatePlayIcon()
  $('btnInfer').classList.add('hidden')
  $('btnPause').classList.remove('hidden')
  $('btnStop').classList.remove('hidden')
  $('live').style.visibility = 'visible'
  vid.style.display = 'none'
  cvs.style.display = 'block'
  log(`Inference started (${targetFps} fps, batch=${MAX_INFLIGHT})`, 'success')
  ws.send(JSON.stringify({ type: 'reset', request_id: 'reset_start' }))
}

function pauseInfer() {
  paused = !paused
  $('btnPause').textContent = paused ? 'Resume' : 'Pause'
  if (!paused) pumpFrames()
}

function stopInfer(logStop = true) {
  streaming = false
  paused = false
  waitingForResetAck = false
  seekBusy = false
  inflightFrames.clear()
  $('btnInfer').classList.remove('hidden')
  $('btnPause').classList.add('hidden')
  $('btnStop').classList.add('hidden')
  $('btnPause').textContent = 'Pause'
  $('live').style.visibility = 'hidden'
  vid.style.display = 'block'
  cvs.style.display = 'none'
  updatePlayIcon()
  if (logStop) log(`Stopped at frame ${frameId}`)
}

function pumpFrames() {
  if (waitingForResetAck || !streaming || paused || seekBusy || inflightFrames.size >= MAX_INFLIGHT) return
  if (nextFrameTime >= vid.duration - 0.05) {
    if (inflightFrames.size === 0) {
      stopInfer(false)
      log('Completed all frames', 'success')
    }
    return
  }

  seekBusy = true
  const captureTime = nextFrameTime
  vid.currentTime = captureTime
  vid.addEventListener('seeked', function handler() {
    vid.removeEventListener('seeked', handler)
    seekBusy = false
    if (!streaming) return
    capCx.drawImage(vid, 0, 0, cap.width, cap.height)
    const frameSnapshot = capCx.getImageData(0, 0, cap.width, cap.height)
    cap.toBlob(async (blob) => {
      if (!blob || !streaming) return
      frameId += 1
      const rid = `f_${frameId}`
      inflightFrames.set(rid, { t0: performance.now(), frameSnapshot, captureTime })
      const header = buildRequestHeader()
      header.request_id = rid
      header.frame_id = frameId
      ws.send(JSON.stringify(header))
      ws.send(await blob.arrayBuffer())
      nextFrameTime = captureTime + 1 / targetFps
      pumpFrames()
    }, 'image/jpeg', 0.5)
  })
}

function measureLabel(text) {
  if (labelWidthCache.has(text)) return labelWidthCache.get(text)
  const width = cx.measureText(text).width + 8
  labelWidthCache.set(text, width)
  return width
}

function imageToGround(px, py) {
  if (!homographyMatrix) return null
  let ux = px, uy = py
  if (undistortEnabled && cameraMatrix && distCoeffs) {
    ;[ux, uy] = undistortPoint(px, py, cameraMatrix, distCoeffs)
  }
  return perspectiveTransform(ux, uy, homographyMatrix)
}

function pruneVehicleTracks(currentFrameId) {
  for (const tid of Object.keys(vehicleTracks)) {
    if (currentFrameId - vehicleTracks[tid].lastFrame > 300) {
      delete vehicleTracks[tid]
      if (selectedVehicle === tid) selectedVehicle = null
    }
  }
}

function pointInPolygon(px, py, poly) {
  let inside = false
  for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
    const xi = poly[i][0], yi = poly[i][1], xj = poly[j][0], yj = poly[j][1]
    if ((yi > py) !== (yj > py) && px < ((xj - xi) * (py - yi)) / (yj - yi) + xi) inside = !inside
  }
  return inside
}

function isVehicleStationary(vid, minFrames = 30, maxDisp = 15) {
  const tr = vehicleTracks[vid]
  if (!tr || tr.positions.length < minFrames) return false
  const recent = tr.positions.slice(-minFrames)
  let total = 0
  for (let i = 1; i < recent.length; i++) {
    const dx = recent[i][0] - recent[i - 1][0]
    const dy = recent[i][1] - recent[i - 1][1]
    total += Math.sqrt(dx * dx + dy * dy)
  }
  return total < maxDisp
}

function patchLaneUI() {
  const host = $('laneList')
  host.innerHTML = ''
  for (const lid in lanes) {
    const lane = lanes[lid]
    const isDrawing = drawingLane === lid
    const div = document.createElement('div')
    div.className = 'lane-item'
    div.innerHTML = `
      <div class="lane-hdr">
        <div class="lane-color" style="background:${lane.color}"></div>
        <span class="lane-name">${lane.name}</span>
        ${isDrawing ? `<button class="btn btn-sm btn-g" data-done="${lid}">Done (${lane.points.length}pts)</button>` : `<button class="btn btn-sm" data-draw="${lid}">Draw</button>`}
        <button class="btn btn-sm" data-reset="${lid}">Reset</button>
        <button class="btn btn-sm btn-r" data-del="${lid}">Del</button>
      </div>
      <div class="lane-stat-row">
        <div class="lane-stat">In: <b>${lane.flowCount}</b></div>
        <div class="lane-stat">Total: <b>${lane.passCount}</b></div>
        <div class="lane-stat">Queue: <b>${lane.queueCount}</b></div>
        <div class="lane-stat">Rate: <b>${lane.flowRate.toFixed(0)}</b> veh/h</div>
      </div>`
    host.appendChild(div)
  }
}

function updateLaneStats(vehicleId, center) {
  for (const lid in lanes) {
    const lane = lanes[lid]
    if (!lane.visible || !lane.polygon || lane.polygon.length < 3) continue
    const isInside = pointInPolygon(center[0], center[1], lane.polygon)
    if (isInside) {
      lane.vehiclesInside.add(vehicleId)
      if (!lane.counted.has(vehicleId)) {
        lane.counted.add(vehicleId)
        lane.passCount += 1
        lane.passTimestamps.push(performance.now())
      }
    } else {
      lane.vehiclesInside.delete(vehicleId)
    }
    lane.flowCount = lane.vehiclesInside.size
    let queueCount = 0
    for (const vid of lane.vehiclesInside) {
      if (isVehicleStationary(vid)) queueCount += 1
    }
    lane.queueCount = queueCount
    const now = performance.now()
    const window = 5 * 60 * 1000
    lane.passTimestamps = lane.passTimestamps.filter((t) => now - t < window)
    lane.flowRate = lane.passTimestamps.length / (window / 3600000)
  }
}

function updateVehicleList() {
  const list = $('vList')
  const ids = Object.keys(vehicleTracks).sort((a, b) => +a - +b)
  $('vEmpty').style.display = ids.length ? 'none' : 'block'
  list.innerHTML = ''
  for (const tid of ids) {
    const tr = vehicleTracks[tid]
    const li = document.createElement('li')
    li.className = 'v-item' + (selectedVehicle === tid ? ' active' : '')
    li.innerHTML = `<div class="v-color" style="background:${colors[tr.classId % colors.length]}"></div><span class="v-id">ID:${tid}</span><span class="v-class">${tr.className}</span>`
    li.onclick = () => {
      selectedVehicle = selectedVehicle === tid ? null : tid
      updateVehicleList()
    }
    list.appendChild(li)
  }

  if (selectedVehicle && vehicleTracks[selectedVehicle]) {
    const tr = vehicleTracks[selectedVehicle]
    $('vDetail').style.display = 'block'
    $('vDetailId').textContent = `ID:${selectedVehicle}`
    const rows = []
    rows.push(['Class', tr.className])
    rows.push(['Track Points', String(tr.positions.length)])
    if (tr.positions.length) {
      const p = tr.positions[tr.positions.length - 1]
      rows.push(['Pixel X', p[0].toFixed(1)])
      rows.push(['Pixel Y', p[1].toFixed(1)])
    }
    $('vDetailBody').innerHTML = rows.map(([k, v]) => `<div class="v-detail-row"><span class="lbl">${k}</span><span class="val">${v}</span></div>`).join('')
  } else {
    $('vDetail').style.display = 'none'
  }
}

function drawLanes() {
  for (const lid in lanes) {
    const lane = lanes[lid]
    if (!lane.visible || !lane.polygon || lane.polygon.length < 3) continue
    const pts = lane.polygon
    cx.save()
    cx.globalAlpha = 0.2
    cx.fillStyle = lane.color
    cx.beginPath()
    cx.moveTo(pts[0][0], pts[0][1])
    for (let i = 1; i < pts.length; i++) cx.lineTo(pts[i][0], pts[i][1])
    cx.closePath()
    cx.fill()
    cx.restore()
    cx.strokeStyle = lane.color
    cx.lineWidth = 2
    cx.beginPath()
    cx.moveTo(pts[0][0], pts[0][1])
    for (let i = 1; i < pts.length; i++) cx.lineTo(pts[i][0], pts[i][1])
    cx.closePath()
    cx.stroke()
  }
}

function draw(detections, currentFrameId) {
  cx.drawImage(cap, 0, 0, cvs.width, cvs.height)
  const lw = Math.max(2, Math.round(cvs.width / 400))
  const fs = Math.max(13, Math.round(cvs.width / 70))
  cx.lineWidth = lw
  cx.font = `bold ${fs}px Inter,sans-serif`

  for (const d of detections) {
    const color = colors[d.class_id % colors.length]
    cx.strokeStyle = color
    const label = `${d.class_name || d.class_id} ${(d.confidence * 100) | 0}%`
    const width = measureLabel(label)
    const height = fs + 4
    if (d.obb) {
      const p = d.obb.map((v) => v * uploadScale)
      cx.beginPath()
      cx.moveTo(p[0], p[1])
      cx.lineTo(p[2], p[3])
      cx.lineTo(p[4], p[5])
      cx.lineTo(p[6], p[7])
      cx.closePath()
      cx.stroke()
      const labelX = clamp(p[0], 0, Math.max(0, cvs.width - width))
      const labelY = clamp(p[1] - height, 0, Math.max(0, cvs.height - height))
      cx.fillStyle = color
      cx.fillRect(labelX, labelY, width, height)
      cx.fillStyle = '#000'
      cx.fillText(label, labelX + 4, labelY + height - 4)
    } else if (d.bbox) {
      const [x0, y0, w0, h0] = d.bbox
      const x = x0 * uploadScale
      const y = y0 * uploadScale
      const w = w0 * uploadScale
      const h = h0 * uploadScale
      cx.strokeRect(x, y, w, h)
      const labelX = clamp(x, 0, Math.max(0, cvs.width - width))
      const labelY = clamp(y - height, 0, Math.max(0, cvs.height - height))
      cx.fillStyle = color
      cx.fillRect(labelX, labelY, width, height)
      cx.fillStyle = '#000'
      cx.fillText(label, labelX + 4, labelY + height - 4)
    }
  }

  if (trajLength > 0) {
    for (const tid in vehicleTracks) {
      const tr = vehicleTracks[tid]
      if (!tr.positions.length || currentFrameId - tr.lastFrame > 10) continue
      const pts = tr.positions
      const fids = tr.frameIds
      const color = colors[tr.classId % colors.length]
      cx.strokeStyle = color
      cx.lineWidth = Math.max(1, lw - 1)
      cx.beginPath()
      let moved = false
      const start = Math.max(0, pts.length - trajLength)
      for (let i = start; i < pts.length; i++) {
        if (i > start) {
          const gap = fids[i] - fids[i - 1]
          if (gap > 3 || gap <= 0) {
            cx.moveTo(pts[i][0], pts[i][1])
            moved = true
          }
        }
        if (!moved) {
          cx.moveTo(pts[i][0], pts[i][1])
          moved = true
        } else {
          cx.lineTo(pts[i][0], pts[i][1])
        }
      }
      cx.stroke()
    }
    cx.lineWidth = lw
  }

  drawLanes()
}

function onMsg(data) {
  if (typeof data !== 'string') return
  try {
    const response = JSON.parse(data)
    if (response.type === 'pong') return
    if (response.type === 'reset_ack') {
      waitingForResetAck = false
      pumpFrames()
      return
    }
    if (response.type === 'error') {
      if (response.request_id) inflightFrames.delete(response.request_id)
      log(`Error: ${response.error || 'unknown'}`, 'error')
      pumpFrames()
      return
    }
    if (expiredRequests.has(response.request_id)) {
      expiredRequests.delete(response.request_id)
      return
    }
    const entry = inflightFrames.get(response.request_id)
    const endToEnd = entry ? performance.now() - entry.t0 : 0
    if (entry?.frameSnapshot) {
      capCx.putImageData(entry.frameSnapshot, 0, 0)
    }
    inflightFrames.delete(response.request_id)

    const detections = response.detections || []
    const timing = response.timing || {}
    const currentFrameId = response.frame_id || frameId
    fpsCnt += 1
    const elapsed = (performance.now() - fpsT0) / 1000
    $('sLat').textContent = endToEnd.toFixed(0)
    $('sSrv').textContent = (timing.total_ms || 0).toFixed(0)
    $('sOver').textContent = Math.max(0, endToEnd - (timing.total_ms || 0)).toFixed(0)
    $('sFps').textContent = elapsed > 0.5 ? (fpsCnt / elapsed).toFixed(1) : '-'
    $('sDet').textContent = detections.length

    for (const d of detections) {
      if (d.track_id == null || d.track_id < 0) continue
      const tid = String(d.track_id)
      let center = null
      if (d.obb) {
        center = [((d.obb[0] + d.obb[2] + d.obb[4] + d.obb[6]) / 4) * uploadScale, ((d.obb[1] + d.obb[3] + d.obb[5] + d.obb[7]) / 4) * uploadScale]
      } else if (d.bbox) {
        center = [(d.bbox[0] + d.bbox[2] / 2) * uploadScale, (d.bbox[1] + d.bbox[3] / 2) * uploadScale]
      }
      if (!center) continue
      if (!vehicleTracks[tid]) {
        vehicleTracks[tid] = { classId: d.class_id, className: d.class_name || `Class_${d.class_id}`, positions: [], frameIds: [], lastFrame: 0, groundPositions: [] }
      }
      const tr = vehicleTracks[tid]
      tr.positions.push(center)
      tr.frameIds.push(currentFrameId)
      tr.lastFrame = currentFrameId
      const gp = d.ground_x != null && d.ground_y != null ? [d.ground_x, d.ground_y] : imageToGround(center[0], center[1])
      tr.groundPositions.push(gp)
      if (tr.positions.length > trajLength) {
        const cut = tr.positions.length - trajLength
        tr.positions.splice(0, cut)
        tr.frameIds.splice(0, cut)
        tr.groundPositions.splice(0, cut)
      }
      if (laneDetectionEnabled) updateLaneStats(tid, center)
    }

    pruneVehicleTracks(currentFrameId)
    draw(detections, currentFrameId)
    updateVehicleList()
    patchLaneUI()
    $('pf').style.width = `${(nextFrameTime / vid.duration) * 100 || 0}%`
    $('tm').textContent = `${fmtTime(nextFrameTime)} / ${fmtTime(vid.duration)}`
    pumpFrames()
  } catch (error) {
    log(`Parse error: ${error.message}`, 'error')
  }
}

function addLane() {
  laneCounter += 1
  const id = `lane_${laneCounter}`
  lanes[id] = {
    name: `Lane ${laneCounter}`,
    points: [],
    polygon: null,
    visible: false,
    vehiclesInside: new Set(),
    counted: new Set(),
    passCount: 0,
    passTimestamps: [],
    flowCount: 0,
    queueCount: 0,
    flowRate: 0,
    color: laneColors[(laneCounter - 1) % laneColors.length],
  }
  patchLaneUI()
  log(`Added ${lanes[id].name}`, 'success')
}

function startDrawLane(id) {
  if (!lanes[id] || !videoLoaded) return
  drawingLane = id
  lanes[id].points = []
  lanes[id].polygon = null
  lanes[id].visible = false
  $('player').classList.add('lane-drawing')
  patchLaneUI()
}

function finishDrawLane(id) {
  if (!lanes[id] || lanes[id].points.length < 3) return
  lanes[id].polygon = [...lanes[id].points]
  lanes[id].visible = true
  drawingLane = null
  $('player').classList.remove('lane-drawing')
  patchLaneUI()
}

function resetLane(id) {
  if (!lanes[id]) return
  lanes[id].points = []
  lanes[id].polygon = null
  lanes[id].visible = false
  lanes[id].vehiclesInside = new Set()
  lanes[id].counted = new Set()
  lanes[id].passCount = 0
  lanes[id].passTimestamps = []
  lanes[id].flowCount = 0
  lanes[id].queueCount = 0
  lanes[id].flowRate = 0
  if (drawingLane === id) {
    drawingLane = null
    $('player').classList.remove('lane-drawing')
  }
  patchLaneUI()
}

function deleteLane(id) {
  delete lanes[id]
  if (drawingLane === id) drawingLane = null
  $('player').classList.remove('lane-drawing')
  patchLaneUI()
}

function loadHomographyFile(file) {
  if (!file) return
  const reader = new FileReader()
  reader.onload = (event) => {
    $('hInput').value = event.target.result
    log(`Loaded homography file: ${file.name}`)
  }
  reader.readAsText(file)
}

function applyHomography() {
  const text = $('hInput').value.trim()
  const nums = text.split(/[\s,;]+/).map(Number).filter((n) => Number.isFinite(n))
  if (nums.length !== 9) {
    log(`Homography needs exactly 9 numbers, got ${nums.length}`, 'error')
    return
  }
  homographyMatrix = nums
  originLon = +$('oriLon').value
  originLat = +$('oriLat').value
  $('hStatus').textContent = `Homography applied. Origin: ${originLon.toFixed(6)}, ${originLat.toFixed(6)}`
  $('hStatus').style.color = 'var(--accent)'
  log('Homography matrix applied', 'success')
}

function loadCameraFile(file) {
  if (!file) return
  const reader = new FileReader()
  reader.onload = (event) => {
    try {
      const data = JSON.parse(event.target.result)
      const K = data.camera_matrix || data.K
      const dist = data.dist_coeffs || data.dist
      if (!K || !dist) throw new Error('Missing camera_matrix/K or dist_coeffs/dist')
      cameraMatrix = Array.isArray(K[0]) ? K.flat() : K
      distCoeffs = Array.isArray(dist[0]) ? dist.flat() : dist
      $('camStatus').textContent = `Loaded (K:${cameraMatrix.length}, dist:${distCoeffs.length})`
      $('camStatus').style.color = 'var(--accent)'
      $('undistToggle').disabled = false
      log(`Camera params loaded from ${file.name}`, 'success')
    } catch (error) {
      log(`Failed to load camera params: ${error.message}`, 'error')
    }
  }
  reader.readAsText(file)
}

function exportVehicleCsv() {
  const rows = [['track_id', 'frame_id', 'pixel_x', 'pixel_y', 'ground_x', 'ground_y', 'lat', 'lon']]
  for (const tid in vehicleTracks) {
    const tr = vehicleTracks[tid]
    for (let i = 0; i < tr.positions.length; i++) {
      const p = tr.positions[i]
      const gp = tr.groundPositions[i]
      const ll = gp ? groundToLatLon(gp[0], gp[1], originLon, originLat) : null
      rows.push([tid, tr.frameIds[i], p[0].toFixed(1), p[1].toFixed(1), gp ? gp[0].toFixed(3) : '', gp ? gp[1].toFixed(3) : '', ll ? ll[0].toFixed(8) : '', ll ? ll[1].toFixed(8) : ''])
    }
  }
  downloadCsv(rows, 'trackflow_vehicles.csv')
}

function exportLaneCsv() {
  const rows = [['lane_id', 'name', 'pass_count', 'current_in_lane', 'queue_count', 'flow_rate_veh_h']]
  for (const lid in lanes) {
    const l = lanes[lid]
    rows.push([lid, l.name, l.passCount, l.flowCount, l.queueCount, l.flowRate.toFixed(1)])
  }
  downloadCsv(rows, 'trackflow_lanes.csv')
}

function clearAllData() {
  for (const key of Object.keys(vehicleTracks)) delete vehicleTracks[key]
  selectedVehicle = null
  for (const lid of Object.keys(lanes)) resetLane(lid)
  updateVehicleList()
  patchLaneUI()
  log('All data cleared')
}

function setupTabs() {
  document.querySelectorAll('.tab-btn').forEach((btn) => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.tab-btn').forEach((item) => item.classList.remove('active'))
      document.querySelectorAll('.tab-pane').forEach((pane) => pane.classList.remove('active'))
      btn.classList.add('active')
      $(`tab-${btn.dataset.tab}`).classList.add('active')
    })
  })
}

function setupPanZoom() {
  const el = $('player')
  let pan = false
  let sx = 0
  let sy = 0

  el.addEventListener('wheel', (event) => {
    event.preventDefault()
    const rect = el.getBoundingClientRect()
    const xs = (event.clientX - rect.left - targetTransform.panX) / targetTransform.scale
    const ys = (event.clientY - rect.top - targetTransform.panY) / targetTransform.scale
    targetTransform.scale = clamp(targetTransform.scale * (event.deltaY < 0 ? 1.12 : 0.89), 0.5, 12)
    targetTransform.panX = event.clientX - rect.left - xs * targetTransform.scale
    targetTransform.panY = event.clientY - rect.top - ys * targetTransform.scale
    applyTransform()
  })

  el.addEventListener('mousedown', (event) => {
    if (drawingLane) {
      event.preventDefault()
      event.stopPropagation()
      const target = cvs.style.display !== 'none' ? cvs : vid
      const rect = target.getBoundingClientRect()
      const tx = (event.clientX - rect.left - targetTransform.panX) / targetTransform.scale
      const ty = (event.clientY - rect.top - targetTransform.panY) / targetTransform.scale
      const scaleX = (cvs.width || vid.videoWidth || rect.width) / rect.width
      const scaleY = (cvs.height || vid.videoHeight || rect.height) / rect.height
      const point = [tx * scaleX, ty * scaleY]
      lanes[drawingLane].points.push(point)
      patchLaneUI()
      return
    }
    if (event.button === 0) {
      pan = true
      sx = event.clientX - targetTransform.panX
      sy = event.clientY - targetTransform.panY
      el.style.cursor = 'grabbing'
    }
  })

  window.addEventListener('mouseup', () => {
    pan = false
    el.style.cursor = drawingLane ? 'crosshair' : 'grab'
  })
  window.addEventListener('mousemove', (event) => {
    if (!pan) return
    targetTransform.panX = event.clientX - sx
    targetTransform.panY = event.clientY - sy
    applyTransform()
  })
  el.addEventListener('dblclick', resetTransform)
}

function bindEvents() {
  $('cb').onclick = toggleConn
  $('openBtn').onclick = () => $('fi').click()
  $('fi').onchange = (event) => loadVideo(event.target.files[0])
  $('playBtn').onclick = () => { vid.paused ? vid.play() : vid.pause(); updatePlayIcon() }
  $('progress').onclick = (event) => {
    const rect = event.currentTarget.getBoundingClientRect()
    vid.currentTime = ((event.clientX - rect.left) / rect.width) * vid.duration
  }
  $('fpsSel').onchange = () => { targetFps = +$('fpsSel').value }
  $('btnInfer').onclick = startInfer
  $('btnPause').onclick = pauseInfer
  $('btnStop').onclick = () => stopInfer()
  $('clearLogBtn').onclick = (event) => { event.stopPropagation(); logger.clear() }
  $('logHdr').onclick = () => $('logB').classList.toggle('open')
  $('trajLen').onchange = (event) => { trajLength = clamp(+event.target.value || 0, 0, 500) }
  $('loadHBtn').onclick = () => $('hFile').click()
  $('hFile').onchange = (event) => loadHomographyFile(event.target.files[0])
  $('applyHBtn').onclick = applyHomography
  $('loadCamBtn').onclick = () => $('camFile').click()
  $('camFile').onchange = (event) => loadCameraFile(event.target.files[0])
  $('undistToggle').onchange = (event) => { undistortEnabled = event.target.checked }
  $('laneToggle').onchange = (event) => { laneDetectionEnabled = event.target.checked }
  $('addLaneBtn').onclick = addLane
  $('exportVehicleBtn').onclick = exportVehicleCsv
  $('exportLaneBtn').onclick = exportLaneCsv
  $('clearAllBtn').onclick = clearAllData

  $('player').ondragover = (event) => { event.preventDefault(); $('player').classList.add('dragover') }
  $('player').ondragleave = () => $('player').classList.remove('dragover')
  $('player').ondrop = (event) => {
    event.preventDefault()
    $('player').classList.remove('dragover')
    const file = event.dataTransfer?.files?.[0]
    if (file && file.type.startsWith('video/')) loadVideo(file)
  }

  document.addEventListener('dragover', (event) => event.preventDefault())
  document.addEventListener('drop', (event) => event.preventDefault())

  vid.addEventListener('timeupdate', () => {
    $('pf').style.width = `${(vid.currentTime / vid.duration) * 100 || 0}%`
    $('tm').textContent = `${fmtTime(vid.currentTime)} / ${fmtTime(vid.duration)}`
  })

  $('laneList').onclick = (event) => {
    const target = event.target
    if (!(target instanceof HTMLElement)) return
    const drawId = target.dataset.draw
    const doneId = target.dataset.done
    const resetId = target.dataset.reset
    const delId = target.dataset.del
    if (drawId) startDrawLane(drawId)
    if (doneId) finishDrawLane(doneId)
    if (resetId) resetLane(resetId)
    if (delId) deleteLane(delId)
  }

  const resizer = $('resizer')
  const right = document.querySelector('.right-col')
  let startX = 0
  let startW = 0
  resizer.addEventListener('mousedown', (event) => {
    event.preventDefault()
    startX = event.clientX
    startW = right.offsetWidth
    resizer.classList.add('active')
    const onMove = (moveEvent) => {
      const dx = startX - moveEvent.clientX
      right.style.width = `${Math.min(Math.max(startW + dx, 220), window.innerWidth * 0.6)}px`
    }
    const onUp = () => {
      resizer.classList.remove('active')
      document.removeEventListener('mousemove', onMove)
      document.removeEventListener('mouseup', onUp)
    }
    document.addEventListener('mousemove', onMove)
    document.addEventListener('mouseup', onUp)
  })
}

function init() {
  setupTabs()
  setupPanZoom()
  bindEvents()
  patchLaneUI()
  connect(false)
  window.addEventListener('beforeunload', () => {
    stopPing()
    revokeSourceVideo()
    revokeAllUrls()
    if (ws && ws.readyState === WebSocket.OPEN) ws.close()
  })
}

init()
