import fs from 'node:fs';
export function analyze(file,rate=96000,bits=24,channels=2){
  if(![16,24,32].includes(bits)||!Number.isInteger(channels)||channels<1||!Number.isFinite(rate)||rate<=0)throw new Error('Expected integer PCM: 16/24/32 bits, positive rate and channel count');
  const b=fs.readFileSync(file),width=bits/8,frames=Math.floor(b.length/(width*channels));
  const n=65536,hop=32768,re=new Float64Array(n),im=new Float64Array(n),power=new Float64Array(n/2);
  const sample=i=>bits===32?b.readInt32LE(i*channels*width)/2147483648:bits===24?b.readIntLE(i*channels*width,3)/8388608:b.readInt16LE(i*channels*width)/32768;
  let count=0,nonzero=0,peak=0;for(let i=0;i<frames;i++){const s=sample(i);if(s)nonzero++;peak=Math.max(peak,Math.abs(s));}
  for(let start=Math.floor(rate*0.8);start+n<=frames;start+=hop){
    for(let i=0;i<n;i++){re[i]=sample(start+i)*(0.5-0.5*Math.cos(2*Math.PI*i/(n-1)));im[i]=0;}
    for(let i=1,j=0;i<n;i++){let bit=n>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;if(i<j){[re[i],re[j]]=[re[j],re[i]];}}
    for(let len=2;len<=n;len<<=1){const angle=-2*Math.PI/len,cr=Math.cos(angle),ci=Math.sin(angle);for(let start=0;start<n;start+=len){let wr=1,wi=0;for(let j=0;j<len/2;j++){const a=start+j,c=a+len/2,tr=re[c]*wr-im[c]*wi,ti=re[c]*wi+im[c]*wr;re[c]=re[a]-tr;im[c]=im[a]-ti;re[a]+=tr;im[a]+=ti;const next=wr*cr-wi*ci;wi=wr*ci+wi*cr;wr=next;}}}
    for(let i=0;i<n/2;i++)power[i]+=re[i]*re[i]+im[i]*im[i];count++;
  }
  function band(low,high){let sum=0;for(let i=Math.ceil(low*n/rate);i<Math.min(n/2,Math.floor(high*n/rate));i++)sum+=power[i];return sum;}
  const base=band(20,20000),db=(low,high)=>10*Math.log10(band(low,high)/base);
  return {file,rate,bits,channels,frames,seconds:frames/rate,nonzeroFraction:nonzero/frames,peakDbFS:20*Math.log10(peak),fftSize:n,window:'Hann',averages:count,analysis:'left channel; first 0.8s excluded; band power relative to 20Hz-20kHz',band22_24dB:db(22050,24000),band24_40dB:db(24000,40000),band40_46dB:db(40000,46000)};
}
