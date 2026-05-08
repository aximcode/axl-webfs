/** @file
  axl-webfs -- Embedded upload.js asset.

  Served by the `serve` command at /_axl-webfs/upload.js. Drives the
  Upload button and drag-and-drop UI in the directory listing pages
  rendered by dir-list.c. Uses XMLHttpRequest with PUT to the existing
  REST endpoint -- no multipart, no extra server-side parsing.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include <stddef.h>

const char kUploadJs[] =
    "(function(){\n"
    /* Use the current page URL as the upload target so the script
       sees the same path the browser is on -- no separate meta tag
       round-trip through HTML escape / URL decode. */
    "var target=location.pathname;\n"
    "var btn=document.getElementById('axl-upload-btn');\n"
    "var input=document.getElementById('axl-upload-input');\n"
    "var queue=document.getElementById('axl-queue');\n"
    "var card=document.querySelector('.card');\n"
    "if(!btn||!input||!queue||!card)return;\n"
    "var pending=0,anyError=false,reloadTimer=null;\n"
    "function show(){queue.hidden=false;}\n"
    "function cancelReload(){\n"
    "  if(reloadTimer){clearTimeout(reloadTimer);reloadTimer=null;}\n"
    "}\n"
    "function settle(){\n"
    "  if(pending!==0)return;\n"
    "  if(anyError)return;\n"
    "  cancelReload();\n"
    "  reloadTimer=setTimeout(function(){location.reload();},600);\n"
    "}\n"
    "function upload(file){\n"
    "  pending++;cancelReload();show();\n"
    "  var row=document.createElement('div');row.className='queue-row';\n"
    "  var label=document.createElement('span');label.className='queue-name';\n"
    "  label.textContent=file.name;\n"
    "  var bar=document.createElement('progress');bar.max=100;bar.value=0;\n"
    "  row.appendChild(label);row.appendChild(bar);queue.appendChild(row);\n"
    "  var url=target.replace(/\\/+$/,'')+'/'+encodeURIComponent(file.name);\n"
    "  var xhr=new XMLHttpRequest();\n"
    "  xhr.open('PUT',url);\n"
    "  xhr.upload.onprogress=function(e){\n"
    "    if(e.lengthComputable)bar.value=(e.loaded/e.total)*100;\n"
    "  };\n"
    "  function fail(msg){\n"
    "    row.classList.add('err');\n"
    "    label.textContent=file.name+' \\u2014 '+msg;\n"
    "    anyError=true;pending--;settle();\n"
    "  }\n"
    "  xhr.onload=function(){\n"
    "    if(xhr.status>=200&&xhr.status<300){\n"
    "      bar.value=100;pending--;settle();\n"
    "    }else{\n"
    "      fail(xhr.status+' '+(xhr.statusText||'error'));\n"
    "    }\n"
    "  };\n"
    "  xhr.onerror=function(){fail('network error');};\n"
    "  xhr.ontimeout=function(){fail('timeout');};\n"
    "  xhr.send(file);\n"
    "}\n"
    "function enqueue(files){\n"
    "  for(var i=0;i<files.length;i++){\n"
    "    var f=files[i];\n"
    /* Folder drops surface as 0-byte File entries with empty MIME type
       in every major browser; skip them so we don't PUT an empty file
       under the directory's name. */
    "    if(f.size===0&&f.type==='')continue;\n"
    "    upload(f);\n"
    "  }\n"
    "}\n"
    "btn.addEventListener('click',function(){input.click();});\n"
    "input.addEventListener('change',function(){\n"
    "  enqueue(input.files);input.value='';\n"
    "});\n"
    "var depth=0;\n"
    "card.addEventListener('dragenter',function(e){\n"
    "  e.preventDefault();depth++;card.classList.add('dropzone-active');\n"
    "});\n"
    "card.addEventListener('dragover',function(e){e.preventDefault();});\n"
    "card.addEventListener('dragleave',function(){\n"
    "  depth--;if(depth<=0){depth=0;card.classList.remove('dropzone-active');}\n"
    "});\n"
    "card.addEventListener('drop',function(e){\n"
    "  e.preventDefault();depth=0;card.classList.remove('dropzone-active');\n"
    "  if(e.dataTransfer&&e.dataTransfer.files.length)enqueue(e.dataTransfer.files);\n"
    "});\n"
    "})();\n";

const size_t kUploadJsLen = sizeof(kUploadJs) - 1;
