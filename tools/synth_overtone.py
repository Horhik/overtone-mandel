# synthetic overtone singing: drone with one strongly resonated harmonic
import numpy as np, sys
from scipy.io import wavfile
sr=48000
def sing(f0_of_t, ot_of_t, dur, Q=60, gain_db=26, vowel=False):
    t=np.arange(int(dur*sr))/sr
    f0=f0_of_t(t); ph=2*np.pi*np.cumsum(f0)/sr
    y=np.zeros_like(t)
    fot=ot_of_t(t)
    for n in range(1,40):
        fn=n*f0
        amp=1.0/n
        # broad vowel formants
        for fc,bw,g in [(700,130,1.0),(1200,150,0.6),(2600,200,0.3)]:
            amp=amp*(1+g*3/(1+((fn-fc)/bw)**2))
        if not vowel:
            # sharp resonance (overtone singing)
            amp=amp*(1+(10**(gain_db/20)-1)/(1+((fn-fot)/(fot/Q))**2))
        y+=amp*np.sin(n*ph)*(fn<sr/2)
    y+=0.003*np.random.randn(len(t))
    return y/np.abs(y).max()*0.5
segs=[]
# 0-4s: drone 120 Hz, overtone sweeps 700 -> 2400 Hz
segs.append(sing(lambda t:120+0*t, lambda t:700*(2400/700)**(t/4), 4))
segs.append(np.zeros(sr//2))
# 4.5-8.5: pitch glide 100->240 Hz, overtone fixed ~1500
segs.append(sing(lambda t:100*(2.4)**(t/4), lambda t:1500+0*t, 4))
segs.append(np.zeros(sr//2))
# 9-11: plain vowel, no overtone
segs.append(sing(lambda t:150+0*t, lambda t:1000+0*t, 2, vowel=True))
y=np.concatenate(segs)
wavfile.write("voice_test.wav",sr,(y*32767).astype(np.int16))
y.astype(np.float32).tofile("voice_test.raw")  # for tools/voice_cli.c
print(len(y)/sr)
