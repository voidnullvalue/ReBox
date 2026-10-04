(function () {
  var $ = function (id) { return document.getElementById(id); };
  var backend = window.JELLYFIN_BACKEND || "";
  var s = {libs:[], lib:0, libFocus:0, page:0, total:0, items:[], zone:"search", index:0, query:"", submitted:"", searching:false, mode:"browse", playing:false, prior:null, serial:0, authenticated:false, path:[]};
  var hub={source:"media",index:0,ready:false,cameras:[],camera:0,cameraLoading:false};
  var ip={mode:"root",view:"all",group:"",page:0,index:0,rootPage:0,rootIndex:0,query:"",submitted:"",groups:[],channels:[],total:0,serial:0,key:0,loading:false};
  try{var savedIP=JSON.parse(window.localStorage.getItem("media.iptv.state")||"null");if(savedIP){ip.mode=savedIP.mode==="list"?"list":"root";ip.view=/^(all|group|search)$/.test(savedIP.view)?savedIP.view:"all";ip.group=String(savedIP.group||"");ip.page=Math.max(0,parseInt(savedIP.page,10)||0);ip.index=Math.max(0,parseInt(savedIP.index,10)||0);ip.rootPage=Math.max(0,parseInt(savedIP.rootPage,10)||0);ip.rootIndex=Math.max(0,parseInt(savedIP.rootIndex,10)||0);ip.query=String(savedIP.query||"").slice(0,64);ip.submitted=String(savedIP.submitted||"").slice(0,64);}}catch(e){}
  function ipSave(){var checkpoint={mode:ip.mode,view:ip.view,group:ip.group,page:ip.page,index:ip.index,rootPage:ip.rootPage,rootIndex:ip.rootIndex,query:ip.query,submitted:ip.submitted,channelId:ip.channels[ip.index]?ip.channels[ip.index].id:""};try{window.localStorage.setItem("media.iptv.state",JSON.stringify(checkpoint));}catch(e){}api("POST","/api/iptv/state",checkpoint,null,null);}
  function ipFocus(){var a=$("iptvList").getElementsByTagName("button"),i;for(i=0;i<a.length;i++)a[i].className=(i===(ip.mode==="root"?ip.rootIndex:ip.index))?"focus":"";ipSave();}
  function ipRender(){var box=$("iptvList"),i,entries=ip.mode==="root"?[{name:"Search"},{name:"All Channels"}].concat(ip.groups):ip.channels,pg=ip.mode==="root"?ip.rootPage:ip.page,total=ip.mode==="root"?entries.length:ip.total;box.innerHTML="";
    $("iptvHeading").textContent=ip.mode==="root"?"IPTV":(ip.view==="search"?"Results: "+ip.submitted:(ip.view==="group"?ip.group:"All Channels"));
    for(i=0;i<(ip.mode==="root"?Math.min(6,entries.length-pg*6):entries.length);i++)(function(j){var c=entries[ip.mode==="root"?pg*6+j:j];box.appendChild(button(c.name+(ip.mode==="root"&&c.count?" ("+c.count+")":""),"",function(){if(ip.mode==="root")ip.rootIndex=j;else ip.index=j;ipFocus();ipActivate();}));}(i));
    $("iptvPage").textContent="PAGE "+(pg+1)+" / "+Math.max(1,Math.ceil(total/6))+" · LEFT / RIGHT to page · BACK";ipFocus();
  }
  function ipLoad(){if(ip.mode==="root"){ipRender();return;}var serial=++ip.serial;ip.loading=true;show("Loading channels...",true);var path="/api/iptv/channels?limit=6&offset="+(ip.page*6);if(ip.view==="group")path+="&group="+encodeURIComponent(ip.group);if(ip.view==="search")path+="&query="+encodeURIComponent(ip.submitted);
    api("GET",path,null,function(d){if(serial!==ip.serial)return;ip.loading=false;ip.channels=d.channels||[];ip.total=d.total||0;if(ip.page>0&&!ip.channels.length){ip.page=0;ipLoad();return;}ip.index=Math.min(ip.index,Math.max(0,ip.channels.length-1));ipRender();if(hub.source==="iptv"){hideMessage();if(!ip.channels.length)show("No channels found. BACK to groups.",true);}},function(err){ip.loading=false;if(hub.source==="iptv")show("IPTV unavailable: "+err+". BACK to groups.",true);});
  }
  function ipEnter(){ip.loading=true;api("GET","/api/iptv/state",null,function(p){if(p.mode){ip.mode=p.mode==="list"?"list":"root";ip.view=p.view||"all";ip.group=p.group||"";ip.page=p.page||0;ip.index=p.index||0;ip.rootPage=p.rootPage||0;ip.rootIndex=p.rootIndex||0;ip.query=p.query||"";ip.submitted=p.submitted||"";}ipGroups();},ipGroups);}
  function ipGroups(){api("GET","/api/iptv/groups",null,function(d){ip.loading=false;ip.groups=d.groups||[];ip.groups.sort(function(a,b){return a.name<b.name?-1:(a.name>b.name?1:0);});ip.rootPage=Math.min(ip.rootPage,Math.floor((ip.groups.length+1)/6));ipLoad();api("GET","/api/iptv/status",null,function(st){if(st.error&&hub.source==="iptv")show(st.error,true);},function(){});telemetry("iptv-library-"+d.total,null);},function(err){ip.loading=false;show("IPTV library unavailable: "+err,true);});}
  function ipSearch(){ip.mode="keyboard";ip.key=0;$("iptvView").className="hidden";$("searchView").className="";$("queryLabel").textContent="SEARCH CHANNEL";showQuery(ip.query||"Type a channel");setSoftInput(softInput);ipKeyboardFocus();}
  function ipKeyboardFocus(){for(var i=0;i<keyButtons.length;i++)keyButtons[i].className=keyButtons[i].className.replace(/\s*focus/g,"");if(keyButtons[ip.key])keyButtons[ip.key].className+=" focus";}
  function ipKeyboard(code){if(code===13){var flat=[],r,c;for(r=0;r<keys.length;r++)for(c=0;c<keys[r].length;c++)flat.push(keys[r][c]);var v=flat[ip.key];if(v==="SEARCH"){if(!ip.query.replace(/^\s+|\s+$/g,""))return;ip.submitted=ip.query;ip.view="search";ip.mode="list";ip.page=0;ip.index=0;$("searchView").className="hidden";$("iptvView").className="";ipLoad();return;}if(v==="DEL")ip.query=ip.query.slice(0,-1);else if(v==="CLEAR")ip.query="";else if(v==="SPACE")ip.query+=" ";else if(ip.query.length<64)ip.query+=v;showQuery(ip.query||"Type a channel");}else{var row=Math.floor(ip.key/7),col=ip.key%7;if(code===37)col=Math.max(0,col-1);if(code===39)col=Math.min(keys[row].length-1,col+1);if(code===38)row=Math.max(0,row-1);if(code===40)row=Math.min(keys.length-1,row+1);ip.key=Math.min(row*7+col,keyButtons.length-1);}ipKeyboardFocus();}
  function ipActivate(){if(ip.loading)return;if(ip.mode==="root"){var at=ip.rootPage*6+ip.rootIndex;if(at===0){ipSearch();return;}ip.view=at===1?"all":"group";ip.group=at>1?ip.groups[at-2].name:"";ip.mode="list";ip.page=0;ip.index=0;ipLoad();return;}var c=ip.channels[ip.index];if(!c)return;ipSave();ip.loading=true;show("Starting "+c.name+"...",true);api("POST","/api/iptv/play",{channelId:c.id},function(){ip.loading=false;telemetry("iptv-playing-"+c.id,null);hideMessage();},function(err){ip.loading=false;if(hub.source==="iptv"){show(err+". Select another channel or BACK.",true);ipFocus();}});}
  function ipBack(){if(ip.loading)api("POST","/api/iptv/stop",{},null,null);if(ip.mode==="keyboard"){$("searchView").className="hidden";$("iptvView").className="";ip.mode="root";ipRender();return;}if(ip.mode==="list"){ip.mode="root";ip.serial++;ip.loading=false;hideMessage();ipRender();return;}sourceView("media");}
  function ipKey(code){if(code===27||code===8||code===461){ipBack();return;}if(ip.mode==="keyboard"){ipKeyboard(code);return;}if(ip.loading)return;var root=ip.mode==="root",n=root?Math.min(6,ip.groups.length+2-ip.rootPage*6):ip.channels.length,idx=root?ip.rootIndex:ip.index,pg=root?ip.rootPage:ip.page,total=root?ip.groups.length+2:ip.total;if(code===13){ipActivate();return;}if(code===38)idx=Math.max(0,idx-1);if(code===40)idx=Math.min(Math.max(0,n-1),idx+1);if(code===37&&pg>0){pg--;idx=0;}if(code===39&&(pg+1)*6<total){pg++;idx=0;}var changed=pg!==(root?ip.rootPage:ip.page);if(root){ip.rootIndex=idx;ip.rootPage=pg;}else{ip.index=idx;ip.page=pg;}if(changed)ipLoad();else ipFocus();}
  var yt={mode:"root",query:"",submitted:"",page:0,index:0,key:0,results:[],more:false,loading:false,serial:0};
  function ytSave(){api("POST","/api/youtube/state",{query:yt.submitted,page:yt.page,index:yt.index,videoId:yt.results[yt.index]?yt.results[yt.index].id:""},null,null);}
  function ytFocus(){var a=$("youtubeList").getElementsByTagName("button"),i;for(i=0;i<a.length;i++)a[i].className=i===yt.index?"focus":"";}
  function ytRender(){var box=$("youtubeList");box.innerHTML="";$("youtubeHeading").textContent=yt.mode==="root"?"YouTube":"Results: "+yt.submitted;var entries=yt.mode==="root"?[{title:"Search"}]:yt.results;for(var i=0;i<entries.length;i++)(function(j){var x=entries[j];box.appendChild(button(x.title+(x.channel?" · "+x.channel:""),"",function(){yt.index=j;ytFocus();ytActivate();}));}(i));$("youtubePage").textContent=yt.mode==="root"?"SELECT to search · BACK to MEDIA":"PAGE "+(yt.page+1)+" · LEFT / RIGHT to page · BACK";ytFocus();}
  function ytLoad(){var serial=++yt.serial;yt.loading=true;show("Searching YouTube...",true);api("GET","/api/youtube/search?q="+encodeURIComponent(yt.submitted)+"&page="+yt.page,null,function(d){if(serial!==yt.serial)return;yt.loading=false;yt.results=d.results||[];yt.more=!!d.hasMore;yt.index=Math.min(yt.index,Math.max(0,yt.results.length-1));ytRender();ytSave();if(hub.source==="youtube"){hideMessage();if(!yt.results.length)show("No videos found. BACK to Search.",true);}},function(err){if(serial!==yt.serial)return;yt.loading=false;if(hub.source==="youtube")show(err+". BACK to Search.",true);});}
  function ytEnter(){yt.serial++;yt.loading=false;yt.mode="root";yt.query="";yt.submitted="";yt.page=0;yt.index=0;yt.results=[];yt.more=false;ytRender();ytSave();api("GET","/api/youtube/status",null,function(st){if(st.error&&hub.source==="youtube")show(st.error,true);},function(){});}
  function ytKeyboardFocus(){for(var i=0;i<keyButtons.length;i++)keyButtons[i].className=keyButtons[i].className.replace(/\s*focus/g,"");if(keyButtons[yt.key])keyButtons[yt.key].className+=" focus";}
  function ytKeyboard(code){if(code===13){var flat=[],r,c;for(r=0;r<keys.length;r++)for(c=0;c<keys[r].length;c++)flat.push(keys[r][c]);var v=flat[yt.key];if(v==="SEARCH"){if(!yt.query.replace(/^\s+|\s+$/g,""))return;yt.submitted=yt.query;yt.mode="results";yt.page=0;yt.index=0;$("searchView").className="hidden";$("youtubeView").className="";ytLoad();return;}if(v==="DEL")yt.query=yt.query.slice(0,-1);else if(v==="CLEAR")yt.query="";else if(v==="SPACE")yt.query+=" ";else if(yt.query.length<64)yt.query+=v;showQuery(yt.query||"Type a video");}else{var row=Math.floor(yt.key/7),col=yt.key%7;if(code===37)col=Math.max(0,col-1);if(code===39)col=Math.min(keys[row].length-1,col+1);if(code===38)row=Math.max(0,row-1);if(code===40)row=Math.min(keys.length-1,row+1);yt.key=Math.min(row*7+col,keyButtons.length-1);}ytKeyboardFocus();}
  function ytActivate(){if(yt.loading)return;if(yt.mode==="root"){yt.mode="keyboard";yt.key=0;$("youtubeView").className="hidden";$("searchView").className="";$("queryLabel").textContent="SEARCH YOUTUBE";showQuery(yt.query||"Type a video");setSoftInput(softInput);ytKeyboardFocus();return;}var x=yt.results[yt.index];if(!x)return;ytSave();yt.loading=true;show("Resolving "+x.title+"...",true);api("POST","/api/youtube/play",{videoId:x.id},function(){yt.loading=false;hideMessage();telemetry("youtube-playing",null);},function(err){yt.loading=false;if(hub.source==="youtube")show(err+". BACK or select another video.",true);});}
  function ytBack(){if(yt.loading){api("POST","/api/youtube/stop",{},null,null);yt.serial++;yt.loading=false;}if(yt.mode!=="root"){$("searchView").className="hidden";$("youtubeView").className="";yt.mode="root";yt.submitted="";yt.page=0;yt.index=0;yt.results=[];yt.more=false;hideMessage();ytRender();ytSave();return;}sourceView("media");}
  function ytKey(code){if(code===27||code===8||code===461){ytBack();return;}if(yt.mode==="keyboard"){ytKeyboard(code);return;}if(yt.loading)return;if(code===13){ytActivate();return;}if(yt.mode==="root")return;if(code===38)yt.index=Math.max(0,yt.index-1);if(code===40)yt.index=Math.min(Math.max(0,yt.results.length-1),yt.index+1);if((code===37&&yt.page>0)||(code===39&&yt.more)){yt.page+=code===37?-1:1;yt.index=0;ytLoad();}else{ytFocus();ytSave();}}

  var quality={levels:[],index:0,selected:0,loading:false};
  function qualityFocus(){var a=$("qualityList").getElementsByTagName("button");for(var i=0;i<a.length;i++)a[i].className=i===quality.index?"focus":"";}
  function qualityRender(d){quality.levels=d.qualityLevels||[];quality.selected=d.settings?d.settings.jellyfinVideoBitrate:0;var box=$("qualityList");box.innerHTML="";
    for(var i=0;i<quality.levels.length;i++)(function(j){var q=quality.levels[j];if(q.bitrate===quality.selected)quality.index=j;box.appendChild(button(q.name+"   "+(q.bitrate/1000000)+" Mbps"+(q.bitrate===quality.selected?"   (selected)":""),"",function(){quality.index=j;qualitySave();}));}(i));qualityFocus();}
  function qualityLoad(){quality.loading=true;api("GET","/api/settings",null,function(d){quality.loading=false;qualityRender(d);},function(err){quality.loading=false;show("Quality settings unavailable: "+err,true);});}
  function qualitySave(){var q=quality.levels[quality.index];if(quality.loading||!q)return;quality.loading=true;api("POST","/api/settings",{jellyfinVideoBitrate:q.bitrate},function(d){quality.loading=false;qualityRender(d);show("Saved. Applies to the next playback request.",false);},function(err){quality.loading=false;show("Could not save quality: "+err,true);});}
  function qualityKey(code){if(code===27||code===8||code===461){sourceView("media");return;}if(quality.loading)return;if(code===38)quality.index=Math.max(0,quality.index-1);else if(code===40)quality.index=Math.min(Math.max(0,quality.levels.length-1),quality.index+1);else if(code===13){qualitySave();return;}qualityFocus();}
  function sourceFocus(){var ids=["sourceJellyfin","sourceFrigate","sourceIPTV","sourceYouTube","sourceSettings"],i;for(i=0;i<ids.length;i++)$(ids[i]).className=i===hub.index?"focus":"";}
  function cameraFocus(){var a=$("cameraList").getElementsByTagName("button"),i;for(i=0;i<a.length;i++)a[i].className=i===hub.camera?"focus":"";if(a[hub.camera])$("cameraList").scrollTop=a[hub.camera].offsetTop-$("cameraList").offsetTop;}
  function cameraSave(){var c=hub.cameras[hub.camera];if(c)try{window.localStorage.setItem("media.frigate.camera",c.id);}catch(e){}}
  function cameraPlay(){var c=hub.cameras[hub.camera];if(!c)return;if(!c.playable){show(c.reason||"No compatible H.264 stream",false);return;}
    cameraSave();show("Starting live view: "+c.name,true);
    api("POST","/api/frigate/play",{cameraId:c.id},function(){s.playing=true;telemetry("frigate-playing",null);hideMessage();},function(err){show("Camera playback failed: "+err,true);});}
  function cameraLoad(){if(hub.cameraLoading)return;hub.cameraLoading=true;text($("cameraList"),"Loading cameras...");
    api("GET","/api/frigate/cameras",null,function(d){hub.cameraLoading=false;var saved="",i,box=$("cameraList");hub.cameras=d.cameras||[];
      try{saved=window.localStorage.getItem("media.frigate.camera")||"";}catch(e){}
      box.innerHTML="";for(i=0;i<hub.cameras.length;i++)(function(j){var c=hub.cameras[j];if(c.id===saved)hub.camera=j;box.appendChild(button(c.name+(c.playable?"":" — unavailable"),"",function(){hub.camera=j;cameraFocus();cameraPlay();}));}(i));
      if(!hub.cameras.length)text(box,"No cameras configured.");hub.camera=Math.min(hub.camera,Math.max(0,hub.cameras.length-1));cameraFocus();telemetry("frigate-cameras-"+hub.cameras.length,null);
    },function(err){hub.cameraLoading=false;text($("cameraList"),"Frigate unavailable: "+err+". BACK to MEDIA; SELECT to retry.");});}
  function sourceView(source){hub.source=source;if(source!=="media")hub.index=source==="jellyfin"?0:(source==="frigate"?1:(source==="iptv"?2:(source==="youtube"?3:4)));hideMessage();
    $("sourceView").className=source==="media"?"":"hidden";
    $("frigateView").className=source==="frigate"?"":"hidden";
    $("iptvView").className=source==="iptv"?"":"hidden";
    $("youtubeView").className=source==="youtube"?"":"hidden";
    $("qualityView").className=source==="settings"?"":"hidden";
    $("brand").textContent=source==="media"?"MEDIA":source.toUpperCase();
    $("searchButton").className=$("accountButton").className=source==="jellyfin"?"":"hidden";
    $("browse").className=source==="jellyfin"&&s.mode==="browse"?"":"hidden";
    $("searchView").className=source==="jellyfin"&&s.mode==="keyboard"?"":"hidden";
    $("authView").className=source==="jellyfin"&&s.mode==="auth"?"":"hidden";
    if(source==="media")sourceFocus();else if(source==="jellyfin"){
      if(hub.ready){focus();return;}hub.ready=true;
      api("GET","/api/auth/status",null,function(data){s.authenticated=!!data.authenticated;if(s.authenticated)loadHome();else openAuth();},function(err){show("Could not check sign-in: "+err,true);});
    }
    if(source==="frigate")cameraLoad();
    if(source==="iptv")ipEnter();
    if(source==="youtube")ytEnter();
    if(source==="settings")qualityLoad();
    telemetry("source-"+source,null);
  }
  function sourceKey(code){if(hub.source==="settings"){qualityKey(code);return;}if(hub.source==="youtube"){ytKey(code);return;}if(hub.source==="iptv"){ipKey(code);return;}if(code===27||code===8||code===461){if(hub.source==="media")api("POST","/api/tv/exit",{},null,function(err){show("Exit failed: "+err,true);});else sourceView("media");return;}
    if(hub.source==="frigate"){if(code===38)hub.camera=Math.max(0,hub.camera-1);else if(code===40)hub.camera=Math.min(Math.max(0,hub.cameras.length-1),hub.camera+1);else if(code===13){if(hub.cameras.length)cameraPlay();else cameraLoad();return;}cameraFocus();cameraSave();return;}
    if(hub.source!=="media")return;
    if(code===38)hub.index=Math.max(0,hub.index-1);else if(code===40)hub.index=Math.min(4,hub.index+1);else if(code===13){sourceView(["jellyfin","frigate","iptv","youtube","settings"][hub.index]);return;}sourceFocus();}
  var pageSize = 6;
  var keys = [["A","B","C","D","E","F","G"],["H","I","J","K","L","M","N"],["O","P","Q","R","S","T","U"],["V","W","X","Y","Z","0","1"],["2","3","4","5","6","7","8"],["9","SPACE","DEL","CLEAR","SEARCH"]];
  var keyButtons = [];
  var messageTimer = null;
  var authTimer = null;
  function api(method,path,body,done,fail){
    var x=new XMLHttpRequest(); x.open(method,backend+path,true);
    if(body!==null)x.setRequestHeader("Content-Type","application/json");
    x.onreadystatechange=function(){if(x.readyState!==4)return;var data={};try{data=JSON.parse(x.responseText||"{}");}catch(e){}
      if(x.status>=200&&x.status<300){if(done)done(data);}else if(fail)fail(data.error||("HTTP "+x.status));};
    x.onerror=function(){if(fail)fail("Network connection failed");};
    x.send(body===null?null:JSON.stringify(body));
  }
  function metric(){var d=document.documentElement,b=document.body;return {innerWidth:window.innerWidth||0,innerHeight:window.innerHeight||0,clientWidth:d.clientWidth,clientHeight:d.clientHeight,screenWidth:window.screen?screen.width:0,screenHeight:window.screen?screen.height:0,bodyOffsetWidth:b.offsetWidth,bodyOffsetHeight:b.offsetHeight,scrollWidth:d.scrollWidth,scrollHeight:d.scrollHeight,rootWidth:$("root").offsetWidth,rootHeight:$("root").offsetHeight};}
  function bridgeKeys(){var names=[],name;try{if(window.dtv)for(name in window.dtv)names.push(name);}catch(e){}return names.slice(0,40).join(",");}
  function telemetry(type,e){var p=metric();p.type=type;p.keyCode=e?(e.keyCode||0):0;p.which=e?(e.which||0):0;p.key=e&&e.key?e.key:"";if(type==="load")p.bridgeKeys=bridgeKeys();api("POST","/api/tv/event",p,null,null);}
  function show(msg,persistent){clearTimeout(messageTimer);$("message").textContent=msg;$("message").className="";if(!persistent)messageTimer=setTimeout(function(){$("message").className="hidden";},2800);}
  function hideMessage(){$("message").className="hidden";clearTimeout(messageTimer);}
  function text(node,value){node.textContent=value===null||value===undefined?"":String(value);}
  /* HR54 ITV WebKit lacks both; other browsers use the text field. */
  var isDvrWebKit=(typeof ArrayBuffer==="undefined")&&(typeof Uint8Array==="undefined");
  var softInput=!isDvrWebKit;
  function queryValue(){return hub.source==="youtube"?yt.query:(hub.source==="iptv"?ip.query:s.query);}
  function setSoftInput(on){
    var box=$("queryInput");
    $("keyboard").className=on?"hidden":"";
    box.className=on?"":"hidden";
    $("query").className=on?"hidden":"";
    if(on){box.value=queryValue();box.placeholder=$("query").textContent;try{box.focus();}catch(e){}}
  }
  function showQuery(value){
    text($("query"),value);
    if($("queryInput").className==="") $("queryInput").value=queryValue();
  }
  /* Submit through the existing OSK handlers. */
  function keyIndexOf(label){for(var i=0,n=0;i<keys.length;i++){for(var c=0;c<keys[i].length;c++,n++)if(keys[i][c]===label)return n;}return -1;}
  function pressLabel(label){
    var at=keyIndexOf(label);if(at<0)return;
    if(hub.source==="youtube"){yt.key=at;ytKeyboard(13);return;}
    if(hub.source==="iptv"){ip.key=at;ipKeyboard(13);return;}
    s.index=at;activateKey();
  }
  function softSync(){
    var v=$("queryInput").value;if(v.length>64)v=v.slice(0,64);
    if(hub.source==="youtube")yt.query=v;else if(hub.source==="iptv")ip.query=v;else s.query=v;
    text($("query"),v);
  }
  function softKey(e){
    e=e||window.event;
    var code=e.keyCode||e.which;
    /* Enter runs the same search as the SEARCH key; everything else is
     * ordinary typing, which must reach the field rather than the
     * navigation handlers. */
    if(code===13){softSync();pressLabel("SEARCH");if(e.preventDefault)e.preventDefault();return false;}
    return true;
  }
  /* BACK from the field means whatever BACK means for the open source. */
  function softBack(){
    if(hub.source==="settings"){sourceView("media");return;}
    if(hub.source==="youtube"){ytBack();return;}
    if(hub.source==="iptv"){ipBack();return;}
    back();
  }
  function checkpoint(){return {lib:s.libs[s.lib]?s.libs[s.lib].id:null,page:s.page,searching:s.searching,submitted:s.submitted,query:s.query,zone:s.zone,index:s.index,path:s.path};}
  function saveState(){var p=checkpoint();try{window.localStorage.setItem("jellyfin.tv.state",JSON.stringify(p));}catch(e){}api("POST","/api/tv/state",p,null,null);}
  function restoreState(p){var i;if(!p||!p.zone){try{p=JSON.parse(window.localStorage.getItem("jellyfin.tv.state")||"null");}catch(e){}}if(!p)return;
    for(i=0;i<s.libs.length;i++)if(s.libs[i].id===p.lib){s.lib=i;s.libFocus=i;break;}
    s.page=Math.max(0,parseInt(p.page,10)||0);s.searching=!!p.searching;s.submitted=p.submitted||"";s.query=p.query||"";s.zone=p.zone==="card"?"card":"library";s.index=Math.max(0,parseInt(p.index,10)||0);
    s.path=Array.isArray(p.path)?p.path.filter(function(x){return x&&x.id;}).slice(0,8):[];}
  function button(label,cls,action){var b=document.createElement("button");b.type="button";b.className=cls||"";text(b,label);b.onclick=action;return b;}
  function focus(){if(hub.source!=="jellyfin"){$("browse").className=$("searchView").className=$("authView").className="hidden";return;}var nodes=$("root").getElementsByTagName("button"),i,n;for(i=0;i<nodes.length;i++)nodes[i].className=nodes[i].className.replace(/\s*focus/g,"");
    if(s.mode==="auth")n=$("authAction");else if(s.mode==="keyboard")n=keyButtons[s.index];else if(s.zone==="account")n=$("accountButton");else if(s.zone==="search")n=$("searchButton");else if(s.zone==="library")n=$("libraries").getElementsByTagName("button")[s.libFocus];else if(s.zone==="card")n=$("items").getElementsByTagName("button")[s.index];else if(s.zone==="pager")n=s.index===0?$("prev"):$("next");
    if(n){n.className+=" focus";if(s.zone==="library"){$("libraries").scrollLeft=n.offsetLeft-$("libraries").offsetLeft-20;}}}
  function setFocus(zone,index){s.zone=zone;s.index=index||0;focus();}
  function renderLibraries(){var box=$("libraries");box.innerHTML="";for(var i=0;i<s.libs.length;i++)(function(j){var b=button(s.libs[j].name,"",function(){selectLibrary(j);});box.appendChild(b);}(i));}
  function selectLibrary(i){s.lib=i;s.libFocus=i;s.page=0;s.searching=false;s.submitted="";saveState();loadItems("card",0);}
  function renderItems(){var box=$("items");box.innerHTML="";for(var i=0;i<s.items.length;i++)(function(j){var it=s.items[j],b=button("","card"+(it.playable?"":" unplayable")+(it.isFolder?" folder":""),function(){activate(j);});var title=document.createElement("span"),meta=document.createElement("span");title.className="title";meta.className="meta";text(title,it.name);if(it.isFolder){var n=it.childCount;text(meta,(n===null||n===undefined||n<0)?"SELECT TO OPEN":(n+" item"+(n===1?"":"s")+"  ·  SELECT TO OPEN"));}else text(meta,(it.year?it.year+"  ·  ":"")+(it.playable?"SELECT TO PLAY":it.type||"BROWSE ONLY"));b.appendChild(title);b.appendChild(meta);box.appendChild(b);}(i));
    var lib=s.libs[s.lib];var crumb=lib?lib.name:"All titles";for(var p=0;p<s.path.length;p++)crumb+="  ›  "+s.path[p].name;$("heading").textContent=s.searching?'Results: '+s.submitted:crumb;
    $("page").textContent="PAGE "+(s.page+1)+" / "+Math.max(1,Math.ceil(s.total/pageSize));
    $("prev").disabled=s.page===0;$("next").disabled=(s.page+1)*pageSize>=s.total;
    var libs=$("libraries").getElementsByTagName("button");for(i=0;i<libs.length;i++)libs[i].className=i===s.lib?"active":"";
  }
  function loadItems(zone,index){var serial=++s.serial,lib=s.libs[s.lib],parent=null;
    /* Browse the open container rather than the library root, so the
     * organization the library already has survives instead of being flattened.
     * Only the un-parented "All" view stays flat. */
    if(s.path.length)parent=s.path[s.path.length-1].id;else if(lib&&lib.id)parent=lib.id;
    var path="/api/items?limit="+pageSize+"&offset="+(s.page*pageSize);
    if(parent)path+="&parent="+encodeURIComponent(parent);else path+="&videoOnly=1";
    if(s.searching&&s.submitted)path+="&search="+encodeURIComponent(s.submitted);
    show("Loading titles...",true);api("GET",path,null,function(data){if(serial!==s.serial)return;s.items=data.items||[];s.total=data.total||0;renderItems();hideMessage();
      if(!s.items.length){show(s.searching?"No results for "+s.submitted:"Nothing in here",true);setFocus("search",0);}
      else setFocus(zone==="library"?"library":"card",Math.min(index||0,s.items.length-1));saveState();},function(err){show("Could not load titles: "+err,true);});}
  function page(delta,idx){var next=s.page+delta;if(next<0||next*pageSize>=s.total)return;s.page=next;saveState();loadItems("card",idx||0);}
  /* A card is either a container to open or media to play. */
  function activate(i){var it=s.items[i];if(!it)return;
    if(it.isFolder&&it.id){s.path.push({id:it.id,name:it.name});s.page=0;saveState();loadItems("card",0);return;}
    play(i);}
  function play(i){var it=s.items[i];if(!it)return;if(!it.playable){show("This title has no playable video file.",false);return;}
    setFocus("card",i);saveState();show("Starting "+it.name+"...",true);api("POST","/api/play",{itemId:it.id,returnToTv:true},function(){s.playing=true;show("Playing "+it.name,false);},function(err){show("Playback failed: "+err,true);});}
  function renderKeyboard(){var box=$("keyboard");box.innerHTML="";keyButtons=[];for(var r=0;r<keys.length;r++){var row=document.createElement("div");row.className="keyrow";for(var c=0;c<keys[r].length;c++)(function(label,index){var b=button(label,"key"+(label.length>1?" wide":""),function(){if(hub.source==="youtube"){yt.key=index;ytKeyboard(13);}else if(hub.source==="iptv"){ip.key=index;ipKeyboard(13);}else{s.index=index;activateKey();}});row.appendChild(b);keyButtons.push(b);}(keys[r][c],keyButtons.length));box.appendChild(row);}}
  function openSearch(){$("queryLabel").textContent="SEARCH TITLE";s.prior={lib:s.lib,page:s.page,searching:s.searching,submitted:s.submitted,index:s.index,zone:s.zone,path:s.path};s.mode="keyboard";$("browse").className="hidden";$("searchView").className="";showQuery(s.query||"Type a title");setSoftInput(softInput);setFocus("keyboard",0);hideMessage();}
  function closeSearch(){s.mode="browse";$("searchView").className="hidden";$("browse").className="";var p=s.prior;if(p){s.lib=p.lib;s.libFocus=p.lib;s.page=p.page;s.searching=p.searching;s.submitted=p.submitted;s.path=p.path||[];loadItems(p.zone,p.index);}else focus();}
  function submitSearch(){if(!s.query.replace(/^\s+|\s+$/g,"")){show("Enter a title first.",false);return;}s.submitted=s.query.replace(/^\s+|\s+$/g,"");s.searching=true;s.lib=0;s.libFocus=0;s.page=0;s.path=[];s.mode="browse";$("searchView").className="hidden";$("browse").className="";saveState();loadItems("card",0);}
  function activateKey(){var flat=[],r,c;for(r=0;r<keys.length;r++)for(c=0;c<keys[r].length;c++)flat.push(keys[r][c]);var v=flat[s.index];if(v==="SEARCH")return submitSearch();if(v==="DEL")s.query=s.query.slice(0,-1);else if(v==="CLEAR")s.query="";else if(v==="SPACE")s.query+=" ";else if(s.query.length<64)s.query+=v;showQuery(s.query||"Type a title");focus();}
  function moveKeyboard(code){var row=Math.floor(s.index/7),col=s.index%7,next=row,at=col;if(code===37)at=Math.max(0,col-1);if(code===39)at=Math.min(keys[row].length-1,col+1);if(code===38)next=Math.max(0,row-1);if(code===40)next=Math.min(keys.length-1,row+1);s.index=Math.min(next*7+at,keyButtons.length-1);focus();}
  function loadHome(){api("GET","/api/libraries",null,function(data){s.libs=[{id:null,name:"All"}].concat(data.libraries||[]);
    api("GET","/api/tv/state",null,function(p){restoreState(p);renderLibraries();loadItems(s.zone,s.index);},function(){restoreState(null);renderLibraries();loadItems(s.zone,s.index);});
  },function(err){show("Could not load libraries: "+err,true);});}
  function openAuth(){s.mode="auth";$("browse").className="hidden";$("searchView").className="hidden";$("authView").className="";
    $("authTitle").textContent=s.authenticated?"JELLYFIN ACCOUNT":"JELLYFIN SIGN IN";
    if(s.authenticated){$("authText").textContent="Signed in to Jellyfin.";$("authCode").textContent="";$("authAction").textContent="SIGN OUT";}
    else{$("authText").textContent="Select GET CODE, then approve it in another Jellyfin app.";$("authCode").textContent="";$("authAction").textContent="GET CODE";}
    focus();}
  function closeAuth(){if(!s.authenticated)return;clearTimeout(authTimer);s.mode="browse";$("authView").className="hidden";$("browse").className="";setFocus("account",0);}
  function pollAuth(){clearTimeout(authTimer);api("GET","/api/auth/poll",null,function(data){if(data.authenticated){s.authenticated=true;clearTimeout(authTimer);$("authView").className="hidden";$("browse").className="";s.mode="browse";setFocus("search",0);loadHome();return;}
      if(data.expired){$("authText").textContent="Code expired. Select GET CODE for a new one.";$("authCode").textContent="";$("authAction").textContent="GET CODE";return;}
      authTimer=setTimeout(pollAuth,3000);},function(err){$("authText").textContent="Waiting for Jellyfin: "+err;authTimer=setTimeout(pollAuth,5000);});}
  function authAction(){if(s.authenticated){api("POST","/api/auth/logout",{},function(){s.authenticated=false;s.libs=[];s.items=[];openAuth();},function(err){show("Sign out failed: "+err,true);});return;}
    $("authText").textContent="Requesting a code...";api("POST","/api/auth/start",{},function(data){$("authText").textContent="Approve this code in another Jellyfin app:";$("authCode").textContent=data.code||"";$("authAction").textContent="NEW CODE";pollAuth();},function(err){$("authText").textContent="Could not start Quick Connect: "+err;});}
  function back(){if(s.mode==="keyboard"){closeSearch();return;}if(s.searching){var p=s.prior;s.searching=false;s.submitted="";s.page=p?p.page:0;s.lib=p?p.lib:s.lib;s.libFocus=s.lib;loadItems("card",p&&p.zone==="card"?p.index:0);return;}
    /* BACK walks out of the folder stack before it walks out of the library. */
    if(s.path.length){var n=s.path[s.path.length-1].name;s.path.pop();s.page=0;saveState();show("Back to "+n,true);loadItems("card",0);return;}
    saveState();sourceView("media");}
  function browseKey(code){if(code===13){if(s.zone==="account")openAuth();else if(s.zone==="search")openSearch();else if(s.zone==="library")selectLibrary(s.libFocus);else if(s.zone==="card")activate(s.index);else if(s.zone==="pager")page(s.index===0?-1:1,0);return;}
    if(s.zone==="account"){if(code===37)setFocus("search",0);else if(code===40)setFocus("library",s.lib);return;}
    if(s.zone==="search"){if(code===39)setFocus("account",0);else if(code===40)setFocus("library",s.lib);return;}
    if(s.zone==="library"){if(code===37&&s.libFocus>0){s.libFocus--;focus();}else if(code===39&&s.libFocus<s.libs.length-1){s.libFocus++;focus();}else if(code===38)setFocus("search",0);else if(code===40&&s.items.length)setFocus("card",0);return;}
    if(s.zone==="card"){var col=s.index%3,row=Math.floor(s.index/3),next=s.index;if(code===37)next--;if(code===39)next++;if(code===38)next-=3;if(code===40)next+=3;
      if(code===38&&row===0){if(s.path.length){back();return;}setFocus("library",s.lib);return;}if(code===40&&next>=s.items.length){if((s.page+1)*pageSize<s.total)page(1,col);else setFocus("pager",1);return;}
      if(code===39&&next>=s.items.length&& (s.page+1)*pageSize<s.total){page(1,0);return;}
      if(code===37&&next<0&&s.page>0){page(-1,pageSize-1);return;}
      if(next>=0&&next<s.items.length)setFocus("card",next);return;}
    if(s.zone==="pager"){if(code===37)setFocus("pager",0);else if(code===39)setFocus("pager",1);else if(code===38&&s.items.length)setFocus("card",s.items.length-1);return;}}
  /* ----------------------------------------------------------------
     DOOM overlay
     ----------------------------------------------------------------
     The engine never owns the TV.  It publishes a finished frame to the
     backend, and we paint that frame into a canvas layered over the UI.
     That keeps every drawing operation inside the WebKit surface that
     already works, instead of a native process that cannot open a
     display.  Frames are palette-indexed and travel as base64 because
     this WebKit is too old for responseType="arraybuffer".

     Entry is driven by the backend: when the engine is running the
     overlay appears, and when it stops the overlay goes away and the
     Jellyfin UI is left exactly as it was. */
  var DOOM_KEYMAP={37:1,39:2,38:4,40:8,13:16,32:32};
  var g={on:false,seq:-1,miss:0,held:0,img:null,pal:null,w:0,h:0,ctx:null,
         cv:null,timer:null,statusTimer:null,
         beat:0,rep:0,ok:0,len:0,stage:"",drew:0,hdrv:""};

  /* This WebKit has no ArrayBuffer, and almost certainly no typed-array
     constructors either, so the decoder must not reference any of them.
     The only typed array available is the one the browser hands us as
     ImageData.data.  That rules out the usual trick of packing a palette
     entry into one 32-bit word, because writing a word would depend on
     the host's byte order on this big-endian box.  So the palette is kept
     as a flat byte list and expanded four bytes at a time.

     Frames arrive base64 encoded, so they must be unpacked before use.
     The character table and scratch buffers are built once.  The pixel
     loop consumes three bytes per four base64 characters straight into
     ImageData.data, so there is no intermediate per-frame buffer. */
  function doomBase64Table(){
    var t=new Array(128),i,c;
    for(i=0;i<128;i++)t[i]=-1;
    for(c=0;c<26;c++)t[65+c]=c;    /* A-Z */
    for(c=0;c<26;c++)t[97+c]=c+26; /* a-z */
    for(c=0;c<10;c++)t[48+c]=c+52; /* 0-9 */
    t[43]=62;t[47]=63;              /* + and / */
    /* '=' is base64 padding, not data.  It must decode to zero bits: the
       final group of a frame is always padded, and letting it contribute
       -1 would turn the whole 24-bit word into -1. */
    t[61]=0;
    return t;
  }

  /* Unpack count bytes starting at raw byte offset startByte into out.
     base64 carries three bytes per four characters, so a start offset
     that is not a multiple of three begins part way into a group.  In
     that first group only the trailing 3-(offset%3) bytes belong to the
     requested range; the bytes before them belong to the previous range
     and must be dropped rather than shifted into place. */
  function doomUnpack(b64,startByte,count,out){
    /* Math.floor matters: 20/3 is 6.67 in JavaScript, and truncating that
       to a character index would read the palette from the wrong offset. */
    var t=g.bt,ci=Math.floor(startByte/3)*4,skip=startByte%3,o=0,i4,v,step;
    while(o<count){
      i4=ci;ci=i4+4;
      v=t[b64.charCodeAt(i4)]<<18|t[b64.charCodeAt(i4+1)]<<12|
        t[b64.charCodeAt(i4+2)]<<6|t[b64.charCodeAt(i4+3)];
      step=skip?3-skip:3;skip=0;
      if(step>0&&o<count)out[o++]=v>>16&255;
      if(step>1&&o<count)out[o++]=v>>8&255;
      if(step>2&&o<count)out[o++]=v&255;
    }
  }

  /* Script errors are only ever visible in the receiver's own log, and the
     calls that matter are wrapped in try/catch, so a throw would be
     completely silent.  This reports decoder failures and a periodic
     heartbeat to the backend so a blank canvas is diagnosable. */
  function doomReport(tag,err){
    try{
      if(err&&g.rep++<3)
        api("POST","/api/doom/caps",{caps:tag,err:(err&&err.message)||String(err)},function(){},function(){});
    }catch(e){}
  }
  function doomBeat(){
    try{
      if(++g.beat%5)return;
      api("POST","/api/doom/caps",{caps:"beat",
        state:"seq="+g.seq+" miss="+g.miss+" ok="+g.ok+
             " len="+g.len+" stage="+g.stage+" hdr["+g.hdrv+"] drew="+(g.drew?1:0)},function(){},function(){});
    }catch(e){}
  }

  function doomApply(b64){
    g.len=b64?b64.length:-1;
    if(!b64||b64.length<24){g.stage="short";return;}
    if(!g.bt){g.bt=doomBase64Table();g.hdr=new Array(20);}
    var hdr=g.hdr;
    doomUnpack(b64,0,20,hdr);
    /* "HRDF", checked on decoded bytes: the base64 text never spells it. */
    if(hdr[0]!==72||hdr[1]!==82||hdr[2]!==68||hdr[3]!==70){
      g.stage="magic:"+hdr[0]+","+hdr[1]+","+hdr[2]+","+hdr[3];return;}
    var ver=hdr[4]|hdr[5]<<8|hdr[6]<<16|hdr[7]<<24;
    var w=hdr[8]|hdr[9]<<8|hdr[10]<<16|hdr[11]<<24;
    var h=hdr[12]|hdr[13]<<8|hdr[14]<<16|hdr[15]<<24;
    var seq=hdr[16]|hdr[17]<<8|hdr[18]<<16|hdr[19]<<24;
    g.hdrv="ver="+ver+" w="+w+" h="+h+" seq="+seq;
    if(ver!==1||w<=0||h<=0){g.stage="dims";return;}
    if(seq===g.seq){g.stage="same";return;}
    if(b64.length<4*Math.ceil((1044+w*h)/3)){
      g.stage="trunc:"+b64.length+"<"+4*Math.ceil((1044+w*h)/3);return;}
    g.seq=seq;g.ok++;
    if(!g.img||g.w!==w||g.h!==h){
      g.w=w;g.h=h;g.cv.width=w;g.cv.height=h;
      g.ctx=g.cv.getContext("2d");
      g.img=g.ctx.createImageData(w,h);
      g.pal=new Array(1024);
    }
    doomUnpack(b64,20,1024,g.pal);
    var d=g.img.data,pal=g.pal,n=w*h,p=0,ci=1392,lim=n-2,i4,v,o0,pi;
    while(p<lim){
      i4=ci;ci=i4+4;
      v=g.bt[b64.charCodeAt(i4)]<<18|g.bt[b64.charCodeAt(i4+1)]<<12|
        g.bt[b64.charCodeAt(i4+2)]<<6|g.bt[b64.charCodeAt(i4+3)];
      o0=p*4;pi=(v>>16&255)*4;
      d[o0]=pal[pi];d[o0+1]=pal[pi+1];d[o0+2]=pal[pi+2];d[o0+3]=pal[pi+3];
      o0+=4;pi=(v>>8&255)*4;
      d[o0]=pal[pi];d[o0+1]=pal[pi+1];d[o0+2]=pal[pi+2];d[o0+3]=pal[pi+3];
      o0+=4;pi=(v&255)*4;
      d[o0]=pal[pi];d[o0+1]=pal[pi+1];d[o0+2]=pal[pi+2];d[o0+3]=pal[pi+3];
      p+=3;
    }
    /* Trailing pixels when w*h is not a multiple of three.  sh selects the
       byte within the current group, so one or two leftover pixels are read
       from the padded final group without duplicating the first byte. */
    var sh=24;
    while(p<n){
      if(sh>16){
        i4=ci;ci=i4+4;
        v=g.bt[b64.charCodeAt(i4)]<<18|g.bt[b64.charCodeAt(i4+1)]<<12|
          g.bt[b64.charCodeAt(i4+2)]<<6|g.bt[b64.charCodeAt(i4+3)];
        sh=16;
      }
      o0=p*4;pi=(v>>sh&255)*4;
      d[o0]=pal[pi];d[o0+1]=pal[pi+1];d[o0+2]=pal[pi+2];d[o0+3]=pal[pi+3];
      p++;sh-=8;
    }
    g.ctx.putImageData(g.img,0,0);
    g.miss=0;g.drew=1;g.stage="drew";
  }

  /* Pull the newest frame.  304 means "nothing new", which is the common
     case while we poll faster than the engine draws, so it costs only a
     tiny response.  An empty gap here is what paces us to the frame rate. */
  function doomPoll(){
    if(!g.on)return;
    try{
    var x=new XMLHttpRequest();
    x.open("GET","/doom/frame?since="+g.seq,true);
    x.onreadystatechange=function(){
      if(x.readyState!==4)return;
      if(!g.on)return;
      if(x.status===200){
        try{doomApply(x.responseText);}catch(e){g.miss++;doomReport("apply",e);}
        g.timer=setTimeout(doomPoll,4);
      }else if(x.status===304){
        g.timer=setTimeout(doomPoll,40);
      }else{
        g.miss++;
        g.timer=setTimeout(doomPoll,g.miss>5?1000:100);
      }
    };
    x.send(null);
    }catch(e){g.miss++;g.timer=setTimeout(doomPoll,500);}
  }

  function doomStatus(){
    try{
      api("GET","/api/doom/status",null,function(d){
        if(g.on){if(!d.running)doomExit();}
        else if(d.running)doomEnter();
        doomBeat();
      },function(){if(g.on)doomExit();});
    }catch(e){}
  }

  function doomEnter(){
    if(g.on)return;
    var cv=document.createElement("canvas");
    cv.id="doomCanvas";
    cv.style.cssText="position:absolute;top:0;left:0;width:100%;height:100%;"+
      "z-index:10;background:#000;image-rendering:pixelated;"+
      "-webkit-image-rendering:pixelated;image-rendering:optimize-contrast";
    $("root").appendChild(cv);
    g.cv=cv;g.on=true;g.seq=-1;g.miss=0;g.held=0;
    g.statusTimer=setInterval(doomStatus,1000);
    doomPoll();
    telemetry("doom-enter",null);
  }

  function doomExit(){
    if(!g.on)return;
    g.on=false;
    if(g.timer){clearTimeout(g.timer);g.timer=null;}
    if(g.statusTimer){clearInterval(g.statusTimer);g.statusTimer=null;}
    if(g.cv&&g.cv.parentNode)g.cv.parentNode.removeChild(g.cv);
    /* Release any held direction so the engine does not keep walking after
       the overlay closes. */
    if(g.held){g.held=0;doomSendInput();}
    g.cv=null;g.ctx=null;g.img=null;g.pal=null;g.w=0;g.h=0;g.seq=-1;
    telemetry("doom-exit",null);
  }

  function doomSendInput(){
    api("POST","/api/doom/input",{keys:g.held},function(){},function(){});
  }

  function doomSetKey(code,down){
    var bit=DOOM_KEYMAP[code];
    if(!bit)return false;
    if(down)g.held|=bit;else g.held&=~bit;
    doomSendInput();
    return true;
  }

  function doomQuit(){
    api("POST","/api/doom/stop",{},function(){},function(){});
    doomExit();
  }

  document.onkeydown=function(e){e=e||window.event;var code=e.keyCode||e.which;
    /* Let the focused browser input handle typing and editing. */
    if(softInput&&$("searchView").className===""&&document.activeElement===$("queryInput")){
      if(code===13)return softKey(e);
      if(code===27||code===461){softBack();if(e.preventDefault)e.preventDefault();return false;}
      return true;
    }
    telemetry("keydown",e);
    if(code===37||code===38||code===39||code===40||code===13||code===27||code===8||code===461||code===83){if(e.preventDefault)e.preventDefault();e.returnValue=false;}
    /* In-game: exit keys quit, the rest become held movement.  This has to
       happen before the STOP guard below, which exists to stop browsing
       STOP from closing ITV. */
    if(g.on){
      try{
        if(code===27||code===8||code===461||code===83){doomQuit();return false;}
        if(doomSetKey(code,true))return false;
      }catch(err){doomExit();}
    }
    if(code===83&&hub.source==="iptv"&&ip.loading){ipBack();return false;}
    if(code===83)return false; /* STOP while browsing must not close ITV. */
    if(hub.source!=="jellyfin"){sourceKey(code);return false;}
    if(s.mode==="auth"){if(code===13)authAction();else if(code===27||code===8||code===461){if(s.authenticated)closeAuth();else sourceView("media");}return false;}
    if(code===27||code===8||code===461){back();return false;}
    if(s.mode==="keyboard"){if(code===13)activateKey();else if(code>=37&&code<=40)moveKeyboard(code);}
    else browseKey(code);return false;};
  document.onkeyup=function(e){e=e||window.event;if(!g.on)return;try{doomSetKey(e.keyCode||e.which,false);}catch(err){doomExit();}};
  window.onblur=function(){if(g.on&&g.held){g.held=0;try{doomSendInput();}catch(e){}}};

  /* Anything that goes wrong in the DOOM layer must never take the
     Jellyfin UI down with it.  The first version of this feature did
     exactly that: a missing ArrayBuffer threw while the IIFE was still
     evaluating, before the UI bootstrap ran, and blanked the whole page.
     So the feature is now started inside a guard, and the UI bootstrap
     below always executes.  The capability probe is reported as a normal
     keydown telemetry event, because the receiver's JS engine reports
     script errors only to its own log and not to the page. */
  try{
    var probe=document.createElement("canvas");
    var pctx=probe.getContext&&probe.getContext("2d");
    g.caps=(pctx&&pctx.createImageData)?"2d":"none";
    api("POST","/api/doom/caps",{arrayBuffer:String(typeof ArrayBuffer),
                                  typed:String(typeof Uint8Array),
                                  imgData:String(!!(pctx&&pctx.createImageData)),
                                  caps:g.caps},function(){},function(){});
    if(g.caps!=="none")setInterval(doomStatus,1000);
  }catch(err){api("POST","/api/doom/caps",{caps:"error"},function(){},function(){});}

  var backSequence=null;
  function remoteBackPoll(){api("GET","/api/tv/input",null,function(d){
    if(backSequence!==null&&d.back!==backSequence)document.onkeydown({keyCode:27,which:27});
    backSequence=d.back;setTimeout(remoteBackPoll,250);
  },function(){setTimeout(remoteBackPoll,2000);});}
  remoteBackPoll();
  window.onresize=function(){telemetry("resize",null);};
  $("backButton").onclick=function(){document.onkeydown({keyCode:27,which:27});};
  $("searchButton").onclick=openSearch;
  $("accountButton").onclick=openAuth;$("authAction").onclick=authAction;
  $("queryInput").oninput=softSync;
  $("queryInput").onchange=softSync;
  $("prev").onclick=function(){page(-1,0);};$("next").onclick=function(){page(1,0);};
  renderKeyboard();telemetry("load",null);setTimeout(function(){telemetry("settled",null);},1000);
  $("sourceJellyfin").onclick=function(){hub.index=0;sourceView("jellyfin");};
  $("sourceFrigate").onclick=function(){hub.index=1;sourceView("frigate");};
  $("sourceIPTV").onclick=function(){hub.index=2;sourceView("iptv");};
  $("sourceSettings").onclick=function(){hub.index=4;sourceView("settings");};
  $("sourceYouTube").onclick=function(){hub.index=3;sourceView("youtube");};
  var returning=/[?&]source=(jellyfin|frigate|iptv|youtube)/.exec(window.location.search||"");
  sourceView(returning?returning[1]:"media");
}());
