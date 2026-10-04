/* Exercise real ES5 frontend navigation and persistence through its DOM/API. */
const fs=require('fs'),vm=require('vm'),assert=require('assert'),path=require('path');
function boot(search,authenticated){
 const nodes={},requests=[],store={},timers=[];
 function node(tag,id){const n={tagName:tag.toUpperCase(),id:id||'',className:'',children:[],style:{},offsetWidth:1280,offsetHeight:720,offsetTop:0,offsetLeft:0,
 appendChild(c){this.children.push(c);c.parentNode=this;},getElementsByTagName(t){return this.children.reduce((a,c)=>a.concat(c.tagName===t.toUpperCase()?[c]:[],c.getElementsByTagName(t)),[]);}};
 Object.defineProperty(n,'innerHTML',{get(){return '';},set(){this.children=[];}});return n;}
 const html=fs.readFileSync(path.join(__dirname,'../static/tv/index.html'),'utf8');
 for(const m of html.matchAll(/<(\w+)[^>]*id="([^"]+)"/g))nodes[m[2]]=node(m[1],m[2]);
 for(const id in nodes)if(id!=='root')nodes.root.appendChild(nodes[id]);
 const document={getElementById:id=>nodes[id],createElement:t=>node(t),documentElement:{clientWidth:1280,clientHeight:720},body:{offsetWidth:1280,offsetHeight:720}};
 const window={location:{search:search||''},localStorage:{setItem:(k,v)=>store[k]=v,getItem:k=>store[k]||null},innerWidth:1280,innerHeight:720};
 let quality=12000000;
 let checkpoint={lib:'lib1',page:2,zone:'card',index:2,query:'DUNE',searching:false,path:[]},back=0;
 function XHR(){this.open=(m,p)=>{this.method=m;this.path=p;};this.setRequestHeader=()=>{};this.send=raw=>{
  let data={},body=raw?JSON.parse(raw):null;requests.push({method:this.method,path:this.path,body});
  if(this.path==='/api/settings'){if(body)quality=body.jellyfinVideoBitrate;data={settings:{jellyfinVideoBitrate:quality},qualityLevels:[{name:'Conservative',bitrate:8000000},{name:'High',bitrate:12000000},{name:'Maximum',bitrate:16000000}]};}
  else if(this.path==='/api/auth/status')data={authenticated:authenticated!==false};
  else if(this.path==='/api/libraries')data={libraries:[{id:'lib1',name:'Movies'}]};
  else if(this.path==='/api/tv/state'){if(body)checkpoint=JSON.parse(JSON.stringify(body));else data=checkpoint;}
  else if(this.path.indexOf('/api/items')===0)data={total:40,items:Array.from({length:6},(_,i)=>({id:'film'+i,name:'Film '+i,playable:true}))};
  else if(this.path==='/api/frigate/cameras')data={cameras:[{id:'actual_camera',name:'actual_camera',playable:true},{id:'offline',name:'offline',playable:false,reason:'offline'}]};
  else if(this.path==='/api/iptv/groups')data={total:2096,groups:Array.from({length:26},(_,i)=>({name:i===25?'Sports':'Group '+String(i).padStart(2,'0'),count:80}))};
  else if(this.path.indexOf('/api/iptv/channels')===0)data={total:80,channels:Array.from({length:6},(_,i)=>({id:'ch-'+i,name:'Channel '+i,group:'Sports'}))};
  else if(this.path==='/api/iptv/status')data={loaded:true,active:false};
  else if(this.path==='/api/youtube/state')data={query:'DVR',page:2,index:1,videoId:'jNQXAC9IVRw'};
  else if(this.path==='/api/youtube/status')data={available:true,active:false};
  else if(this.path.indexOf('/api/youtube/search?')===0)data={hasMore:true,results:Array.from({length:6},(_,i)=>({id:i===1?'jNQXAC9IVRw':'aqz-KE-bpKQ',title:'Video '+i,channel:'Channel'}))};
  else if(this.path==='/api/tv/input')data={back};
  this.status=200;this.responseText=JSON.stringify(Object.assign({ok:true},data));this.readyState=4;if(this.onreadystatechange)this.onreadystatechange();};}
 const ctx={document,window,XMLHttpRequest:XHR,setTimeout:(fn)=>{timers.push(fn);return timers.length;},clearTimeout:()=>{},setInterval:()=>1,clearInterval:()=>{}};
 vm.runInNewContext(fs.readFileSync(path.join(__dirname,'../static/tv/app.js'),'utf8'),ctx);
 return {nodes,requests,store,press(code){document.onkeydown({keyCode:code});},state:()=>checkpoint,bridge(){back++;const f=timers.find(f=>f.name==='remoteBackPoll');assert(f);f();}};
}
const a=boot();assert.equal(a.nodes.brand.textContent,'MEDIA');assert.equal(a.requests.filter(x=>x.path==='/api/libraries').length,0);
a.press(40);a.press(40);a.press(13);assert.equal(a.nodes.brand.textContent,'IPTV');assert(a.requests.some(x=>x.path==='/api/iptv/groups'));assert.equal(a.nodes.iptvList.children[0].textContent,'Search');
a.bridge();assert.equal(a.nodes.brand.textContent,'MEDIA');a.press(38);a.press(13);assert.equal(a.nodes.brand.textContent,'FRIGATE');assert.equal(a.nodes.cameraList.children[0].textContent,'actual_camera');
a.press(40);a.press(13);assert(!a.requests.some(x=>x.path==='/api/frigate/play'));a.press(38);a.press(13);assert.equal(a.requests.find(x=>x.path==='/api/frigate/play').body.cameraId,'actual_camera');a.press(27);a.press(38);a.press(13);
assert.equal(a.nodes.brand.textContent,'JELLYFIN');assert.equal(a.state().page,2);assert.equal(a.state().index,2);assert.equal(a.state().query,'DUNE');const saved=JSON.stringify(a.state());
a.press(27);assert.equal(a.nodes.brand.textContent,'MEDIA');a.press(13);assert.equal(a.nodes.brand.textContent,'JELLYFIN');assert.equal(JSON.stringify(a.state()),saved);assert.equal(a.requests.filter(x=>x.path==='/api/libraries').length,1);
a.press(13);assert(a.requests.some(x=>x.path==='/api/play'&&x.body.returnToTv));a.press(27);a.press(27);assert(a.requests.some(x=>x.path==='/api/tv/exit'));assert(!a.requests.some(x=>x.path==='/api/auth/logout'));
assert.equal(boot('?source=frigate').nodes.brand.textContent,'FRIGATE');assert.equal(boot('?source=jellyfin').nodes.brand.textContent,'JELLYFIN');
const b=boot('',false);b.press(13);assert.equal(b.nodes.authView.className,'');b.press(27);assert.equal(b.nodes.brand.textContent,'MEDIA');
console.log('PASS Media Hub, IPTV root, real camera actions, EXIT bridge, Jellyfin state/auth/playback ownership');

const iptv=boot('?source=iptv');assert.equal(iptv.nodes.brand.textContent,'IPTV');iptv.press(40);iptv.press(13);assert(iptv.requests.some(x=>x.path.indexOf('/api/iptv/channels?')===0));iptv.press(39);iptv.press(39);iptv.press(39);iptv.press(40);iptv.press(13);let req=iptv.requests.find(x=>x.path==='/api/iptv/play');assert(req&&req.body.channelId==='ch-1'&&!req.body.url);let ipstate=JSON.parse(iptv.store['media.iptv.state']);assert.equal(ipstate.page,3);assert.equal(ipstate.index,1);assert.equal(iptv.requests.filter(x=>x.path==='/api/tv/state').length,0);iptv.press(27);assert.equal(iptv.nodes.iptvHeading.textContent,'IPTV');iptv.press(38);iptv.press(13);assert.equal(iptv.nodes.queryLabel.textContent,'SEARCH CHANNEL');iptv.press(13);for(let i=0;i<5;i++)iptv.press(40);for(let i=0;i<4;i++)iptv.press(39);iptv.press(13);assert(iptv.requests.some(x=>x.path.indexOf('&query=A')>0));iptv.press(27);iptv.press(27);assert.equal(iptv.nodes.brand.textContent,'MEDIA');assert(!iptv.requests.some(x=>x.path==='/api/auth/logout'));console.log('PASS IPTV pagination, opaque ID playback, separate checkpoint, shared keyboard search, BACK hierarchy');

const yt=boot('?source=youtube');assert.equal(yt.nodes.brand.textContent,'YOUTUBE');assert.equal(yt.nodes.youtubeList.children[0].textContent,'Search');assert(!yt.requests.some(x=>x.path.indexOf('/api/youtube/search?')===0));yt.press(13);yt.press(13);for(let i=0;i<5;i++)yt.press(40);for(let i=0;i<4;i++)yt.press(39);yt.press(13);assert(yt.requests.some(x=>x.path.indexOf('/api/youtube/search?q=A&page=0')===0));yt.press(40);yt.press(13);let yplay=yt.requests.find(x=>x.path==='/api/youtube/play');assert.deepEqual(yplay.body,{videoId:'jNQXAC9IVRw'});assert(!yt.requests.some(x=>x.path==='/api/tv/state'));yt.press(39);assert(yt.requests.some(x=>x.path.indexOf('/api/youtube/search?q=A&page=1')===0));yt.press(27);assert.equal(yt.nodes.youtubeList.children[0].textContent,'Search');yt.press(13);assert.equal(yt.nodes.queryLabel.textContent,'SEARCH YOUTUBE');yt.press(27);yt.press(27);assert.equal(yt.nodes.brand.textContent,'MEDIA');assert(!yt.requests.some(x=>x.path==='/api/auth/logout'));console.log('PASS YouTube fresh search entry, ID-only playback, paging, keyboard and BACK; Jellyfin state isolation');

const q=boot();for(let i=0;i<4;i++)q.press(40);q.press(13);assert.equal(q.nodes.brand.textContent,'SETTINGS');assert.equal(q.nodes.qualityView.className,'');assert(q.nodes.qualityList.children[1].textContent.indexOf('(selected)')>0);q.press(40);q.press(13);assert.equal(q.requests.filter(x=>x.path==='/api/settings'&&x.method==='POST').pop().body.jellyfinVideoBitrate,16000000);q.press(38);q.press(38);q.press(13);assert.equal(q.requests.filter(x=>x.path==='/api/settings'&&x.method==='POST').pop().body.jellyfinVideoBitrate,8000000);q.press(27);assert.equal(q.nodes.brand.textContent,'MEDIA');q.press(13);assert(q.nodes.qualityList.children[0].textContent.indexOf('(selected)')>0);assert(!q.requests.some(x=>x.path==='/api/tv/state'||x.path==='/api/auth/logout'||x.path==='/api/play'));console.log('PASS remote quality selection, saved reload, BACK and state/auth/playback isolation');
