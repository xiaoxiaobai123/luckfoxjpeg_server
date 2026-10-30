#ifndef WEBPAGE_H
#define WEBPAGE_H

// The browser page: live camera picture with the detection area and the template area drawn on it (drag them with
// the mouse), a form with every setting, Learn / Save buttons and the screen keys.
static const char kPage[] = R"HTML(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Circle checker</title>
<style>
body{font-family:sans-serif;background:#16181d;color:#dde;margin:10px}
h3{margin:4px 0 8px}
.wrap{display:flex;flex-wrap:wrap;gap:14px;align-items:flex-start}
#cam{position:relative;width:640px;max-width:100%;line-height:0}
#cam img,#cam canvas{width:100%;display:block;background:#000}
#cam canvas{position:absolute;left:0;top:0;cursor:crosshair}
.side{min-width:300px}
fieldset{border:1px solid #3a3f4b;border-radius:6px;margin:0 0 10px;padding:6px 10px}
legend{color:#9ab;font-size:13px}
.row{display:flex;align-items:center;justify-content:space-between;margin:4px 0;gap:8px}
.row label{font-size:14px}.row small{color:#789;margin-left:4px}
input[type=number],select{width:110px;font-size:15px;padding:3px;background:#222731;color:#eef;border:1px solid #444b59;border-radius:4px}
button{font-size:15px;margin:3px 3px 3px 0;padding:7px 12px;background:#2c3a55;color:#fff;border:0;border-radius:5px;cursor:pointer}
button.p{background:#2f7d4f}button.w{background:#8a5a1d}
#msg{min-height:20px;color:#8d8;font-size:14px}
#tpl{max-width:96px;max-height:96px;image-rendering:pixelated;background:#000;border:1px solid #444}
.keys button{padding:8px 12px}
.hint{color:#789;font-size:12px;margin:4px 0}
.tag{display:inline-block;width:10px;height:10px;margin-right:4px;border-radius:2px}
</style>
<h3>Circle checker &nbsp;<small style="color:#789;font-weight:normal">设置 / settings</small></h3>
<div class=wrap>
 <div>
  <div id=cam><img id=camimg src=/cam><canvas id=cv width=640 height=480></canvas></div>
  <div class=hint><span class=tag style="background:#ffc800"></span>Detection area 检测区 &nbsp;
   <span class=tag style="background:#00e5ff"></span>Template area 模板区 (template mode) &nbsp; —
   drag a box to move it, drag its lower-right corner to resize. 拖动方框移动，拖右下角改大小。</div>
  <div style="margin-top:8px"><img id=ui src=/ui width=240 style="cursor:pointer;background:#000">
   <div class=keys style="display:inline-block;vertical-align:top;margin-left:10px">
    <div>Screen keys 屏幕按键</div>
    <button onclick="k('ok')">OK</button><button onclick="k('up')">UP</button><button onclick="k('down')">DOWN</button><br>
    <button onclick="k('left')">LEFT</button><button onclick="k('right')">RIGHT</button><button onclick="k('adj')">ADJ x10</button>
   </div></div>
 </div>
 <div class=side>
  <fieldset><legend>Mode 模式</legend><div id=f_mode></div></fieldset>
  <fieldset id=g_main><legend>Detection 检测</legend><div id=f_main></div></fieldset>
  <fieldset id=g_tpl style="display:none"><legend>Template 模板</legend><div id=f_tpl></div>
   <div class=row><span>Learned template 已学模板</span><img id=tpl alt="none"></div>
   <button class=w onclick="learn()">Learn 学习模板</button></fieldset>
  <fieldset><legend>Camera 相机</legend><div id=f_cam></div></fieldset>
  <fieldset><legend>Detection area 检测区</legend><div id=f_roi></div></fieldset>
  <button class=p onclick="save()">Save 保存</button>
  <button onclick="load()">Reload 重新读取</button>
  <div id=msg></div>
 </div>
</div>
<script>
var S={}, F=[
 {k:'mode',t:'Mode 模式',g:'f_mode',sel:[[0,'Circle 圆形'],[1,'Dark hole 暗孔'],[2,'Template 模板']]},
 {k:'target',t:'Count 个数',g:'f_main',m:[0,1]},{k:'min_dia',t:'Min dia 最小直径',g:'f_main',m:[0,1],u:'px'},
 {k:'max_dia',t:'Max dia 最大直径',g:'f_main',m:[0,1],u:'px'},
 {k:'roundness',t:'Roundness 圆度',g:'f_main',m:[0]},{k:'edge',t:'Edge 边缘',g:'f_main',m:[0]},
 {k:'contrast',t:'Contrast 对比',g:'f_main',m:[1]},{k:'shape',t:'Shape 形状%',g:'f_main',m:[1]},
 {k:'tpl_score',t:'Match min 匹配分数',g:'f_tpl',m:[2],u:'30..99'},
 {k:'tpl_count',t:'Match cnt 匹配个数',g:'f_tpl',m:[2],u:'0=any 不限'},
 {k:'tpl_angle',t:'Rotation 旋转 ±°',g:'f_tpl',m:[2],u:'0..10'},
 {k:'tpl_shape',t:'Shape 形状',g:'f_tpl',m:[2],sel:[[0,'Rect 矩形'],[1,'Circle 圆形']]},
 {k:'tpl_x',t:'Tpl x',g:'f_tpl',m:[2]},{k:'tpl_y',t:'Tpl y',g:'f_tpl',m:[2]},
 {k:'tpl_w',t:'Tpl wide 宽',g:'f_tpl',m:[2]},{k:'tpl_h',t:'Tpl high 高',g:'f_tpl',m:[2]},
 {k:'exposure',t:'Exposure 曝光',g:'f_cam',u:'0.1 ms, 0=auto'},{k:'gain',t:'Gain 增益',g:'f_cam',u:'0.1x, 0=auto'},
 {k:'lang',t:'Screen language 屏幕语言',g:'f_cam',sel:[[0,'English'],[1,'中文']]},
 {k:'roi_x',t:'Area x',g:'f_roi'},{k:'roi_y',t:'Area y',g:'f_roi'},{k:'roi_w',t:'Area wide 宽',g:'f_roi'},{k:'roi_h',t:'Area high 高',g:'f_roi'}
];
function $(i){return document.getElementById(i)}
function msg(t,bad){var m=$('msg');m.textContent=t;m.style.color=bad?'#e88':'#8d8'}
function k(n){fetch('/key?k='+n)}
function build(){
 F.forEach(function(f){var d=document.createElement('div');d.className='row';
  var l=document.createElement('label');l.textContent=f.t;if(f.u){var s=document.createElement('small');s.textContent=f.u;l.appendChild(s)}
  var c;
  if(f.sel){c=document.createElement('select');f.sel.forEach(function(o){var p=document.createElement('option');p.value=o[0];p.textContent=o[1];c.appendChild(p)})}
  else{c=document.createElement('input');c.type='number'}
  c.id='i_'+f.k;c.onchange=function(){send(f.k,c.value)};
  d.appendChild(l);d.appendChild(c);$(f.g).appendChild(d);f.row=d});
}
function apply(j){S=j;
 F.forEach(function(f){var c=$('i_'+f.k);if(document.activeElement!==c)c.value=S[f.k];
  if(f.m)f.row.style.display=f.m.indexOf(S.mode)>=0?'':'none'});
 $('g_tpl').style.display=S.mode==2?'':'none';
 $('g_main').style.display=S.mode==2?'none':'';
 draw()}
function load(){fetch('/settings').then(function(r){return r.json()}).then(function(j){apply(j);msg('')})}
function send(k,v){fetch('/set?'+k+'='+encodeURIComponent(v)).then(function(r){return r.json()}).then(apply)}
function sendMany(o){var q=[];for(var a in o)q.push(a+'='+o[a]);fetch('/set?'+q.join('&')).then(function(r){return r.json()}).then(apply)}
function save(){fetch('/save').then(function(r){return r.text()}).then(function(t){msg(t=='ok'?'Saved 已保存':'Save failed 保存失败',t!='ok')})}
function learn(){msg('Learning... 学习中');fetch('/learn').then(function(){setTimeout(function(){load();$('tpl').src='/tpl.png?'+Date.now();msg('Learned 已学习（记得保存）')},1200)})}
// --- boxes on the picture ---
var cv=$('cv'),cx=cv.getContext('2d'),drag=null;
function boxes(){var b=[{n:'roi',x:'roi_x',y:'roi_y',w:'roi_w',h:'roi_h',c:'#ffc800'}];
 if(S.mode==2)b.push({n:'tpl',x:'tpl_x',y:'tpl_y',w:'tpl_w',h:'tpl_h',c:'#00e5ff'});return b}
function draw(){cx.clearRect(0,0,640,480);if(!S.mode&&S.mode!==0)return;
 boxes().forEach(function(b){cx.strokeStyle=b.c;cx.lineWidth=2;
  var x=S[b.x],y=S[b.y],w=S[b.w],h=S[b.h];
  if(b.n=='tpl'&&S.tpl_shape==1){cx.beginPath();cx.ellipse(x+w/2,y+h/2,w/2,h/2,0,0,6.2832);cx.stroke()}
  else cx.strokeRect(x,y,w,h);
  cx.fillStyle=b.c;cx.fillRect(x+w-8,y+h-8,8,8)})}
function pos(e){var r=cv.getBoundingClientRect();return{x:(e.clientX-r.left)*640/r.width,y:(e.clientY-r.top)*480/r.height}}
cv.onmousedown=function(e){var p=pos(e),bs=boxes().reverse();
 for(var i=0;i<bs.length;i++){var b=bs[i],x=S[b.x],y=S[b.y],w=S[b.w],h=S[b.h];
  if(p.x>=x+w-14&&p.x<=x+w+6&&p.y>=y+h-14&&p.y<=y+h+6){drag={b:b,mode:'size',px:p.x,py:p.y,o:[x,y,w,h]};return}
  if(p.x>=x&&p.x<=x+w&&p.y>=y&&p.y<=y+h){drag={b:b,mode:'move',px:p.x,py:p.y,o:[x,y,w,h]};return}}};
window.onmousemove=function(e){if(!drag)return;var p=pos(e),dx=Math.round(p.x-drag.px),dy=Math.round(p.y-drag.py),b=drag.b,o=drag.o;
 if(drag.mode=='move'){S[b.x]=Math.max(0,Math.min(640-o[2],o[0]+dx));S[b.y]=Math.max(0,Math.min(480-o[3],o[1]+dy))}
 else{S[b.w]=Math.max(16,Math.min(640-o[0],o[2]+dx));S[b.h]=Math.max(16,Math.min(480-o[1],o[3]+dy))}draw()};
window.onmouseup=function(){if(!drag)return;var b=drag.b,o={};o[b.x]=S[b.x];o[b.y]=S[b.y];o[b.w]=S[b.w];o[b.h]=S[b.h];drag=null;sendMany(o)};
$('ui').onclick=function(e){var r=this.getBoundingClientRect();
 fetch('/touch?x='+Math.round((e.clientX-r.left)*240/r.width)+'&y='+Math.round((e.clientY-r.top)*284/r.height))};
build();load();$('tpl').src='/tpl.png?'+Date.now();
setInterval(function(){if(!drag&&document.activeElement.tagName!='INPUT'&&document.activeElement.tagName!='SELECT')load()},3000);
</script>)HTML";

#endif // WEBPAGE_H
