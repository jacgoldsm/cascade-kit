const {Cascade, splitmix32}=require('../cascade/rules');
const g=new Cascade(); const rng=splitmix32(7);
for(let k=0;k<300;k++){ let s=g.initialState(rng()); while(!g.isTerminal(s)){ const ms=g.legalMoves(s); const m=ms[rng()%ms.length]; const a=g.toCSN(s); s=g.play(s,m); console.log(a+'|'+g.moveToString(m)+'|'+g.toCSN(s)); } }
