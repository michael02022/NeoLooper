import numpy as np
import scipy
import soundfile
import sys
import os

path = sys.argv[1]
samples, sample_rate = soundfile.read(path)
amp = np.sum(np.abs(samples), axis=1)
maxamp = np.max(amp)
skip = min(i for i in range(len(amp)) if amp[i] > 0.01*maxamp)
first = min(i for i in range(len(amp)) if amp[i] > 0.75*maxamp)
last = max(i for i in range(len(amp)) if amp[i] > 0.75*maxamp)
width = last-first

# The following functions are adapted from https://gist.github.com/endolith/255291. They
# are used to estimate the pitch of a sample.

def parabolic(f, x):
 xv = 1/2. * (f[x-1] - f[x+1]) / (f[x-1] - 2 * f[x] + f[x+1]) + x
 yv = f[x] - 1/4. * (f[x-1] - f[x+1]) * (xv - x)
 return (xv, yv)

def freq_from_autocorr(sig, fs):
 corr = scipy.signal.fftconvolve(sig, sig[::-1], mode='full')
 corr = corr[len(corr)//2:]
 d = np.diff(corr)
 start = np.where(d > 0)[0][0]
 peak = np.argmax(corr[start:]) + start
 px, py = parabolic(corr, peak)
 return fs / px

frequency = freq_from_autocorr(samples[first:last,0], sample_rate)
wavelength = sample_rate/frequency

segments = []
first_seg_end = first+width//2
segments.append(samples[skip:first_seg_end])
segments.append(samples[first_seg_end:first_seg_end+width//4])

num_segments = 10
for i in range(num_segments):
 overlap = segments[-1].shape[0]
 next_overlap = np.random.randint(width//5, width//3)
 if i == num_segments-1:
 start = first
 else:
 start = np.random.randint(first, last-overlap-next_overlap)
 error = [np.sum(np.abs(segments[-1]-samples[start+i:start+i+overlap])) for i in range(int(wavelength)+1)]
 start += np.argmin(error)
 blend = 0.5 - 0.5*np.cos(np.linspace(0, np.pi, overlap))
 blend = np.expand_dims(blend, 1)
 segments[-1] = (1-blend)*segments[-1] + blend*samples[start:start+overlap]
 if i == num_segments-1:
 next_overlap = first_seg_end-start-overlap
 segments.append(samples[start+overlap:start+overlap+next_overlap])

print(f'loop_start={segments[0].shape[0]}')
result = np.concatenate(segments)
print(f'loop_end={result.shape[0]-1}')
soundfile.write('output.flac', result, sample_rate)