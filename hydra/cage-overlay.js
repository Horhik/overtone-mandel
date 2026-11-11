// Hydra: the visualizer window + a looping video with slowly breathing opacity.
//
// 1. put the video next to the repo:  mkdir -p media && ln -s /path/to/video.mp4 media/cage.mp4
// 2. serve it:                         python3 tools/serve.py media      (http://localhost:8765/)
// 3. start overtone_mandel, press H to hide its interface
// 4. paste this into Hydra (hydra.ojack.xyz), choose the overtone_mandel window in the dialog

// ---- sources: s0 = visualizer window, s1 = video (looped, muted: browsers block autoplay with sound)
s0.initScreen()
const v = document.createElement('video')
v.src = 'http://localhost:8765/cage.mp4'
v.crossOrigin = 'anonymous'      // without it WebGL refuses to read the frames
v.loop = true
v.muted = true
v.playsInline = true
v.play()
s1.init({ src: v, dynamic: true })

// ---- video opacity: breathes very slowly between ~0.2 and ~0.6 (period ~2 min, two waves so it never quite repeats)
const op = () => 0.4 + 0.14 * Math.sin(time * 0.052) + 0.06 * Math.sin(time * 0.019 + 1.3)

// the video, toned into the dark forest palette, drifting a little
const film = src(s1)
  .saturate(0.25)
  .contrast(1.35)
  .color(0.55, 0.95, 0.6)
  .scale(() => 1.04 + 0.03 * Math.sin(time * 0.031))
  .modulate(noise(1.5, 0.03), 0.012)

// ---- A: video UNDER the visuals: shows through the dark areas, bright veins stay on top
src(s0)
  .add(film, op)
  .out()

// ---- B: video OVER the visuals, half-transparent (uncomment to use instead of A)
// src(s0)
//   .blend(film, op)
//   .out()

// ---- optional: let the video slowly warp the visuals too
// src(s0).modulate(src(s1), () => 0.006 + 0.006 * Math.sin(time * 0.04)).add(film, op).out()
