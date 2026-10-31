// SPDX-License-Identifier: GPL-3.0-only
// Exercise the exact GitHub-script policy with mocked REST responses.
import fs from 'node:fs';
const current=fs.readFileSync(new URL('../../.github/workflows/libvirtualhid-experimental.yml', import.meta.url),'utf8');
const lines=current.split('\n');
const start=lines.indexOf('          script: |');
if(start<0)throw Error('Cannot locate the artifact policy script');
const body=[];
for(const line of lines.slice(start+1)){
 if(line.trim() && !line.startsWith('            '))break;
 body.push(line.slice(12));
}
const AsyncFunction=Object.getPrototypeOf(async function(){}).constructor;
const script=new AsyncFunction('core','context','github',body.join('\n'));
const previous=current.replace(/^  kernel:$/m,'  kernel:\n    # A runtime-only fixture change');
if(previous===current)throw Error('Cannot locate the runtime job fixture');
const workflow='.github/workflows/libvirtualhid-experimental.yml';
const cases=[
 {name:'runtime harness',old:previous,reuse:true},
 {name:'shell fixture',files:['scripts/session-broker-test.sh'],reuse:true},
 {name:'Python fixture',files:['scripts/session-broker-concurrency-test.py'],reuse:true},
 {name:'new policy regression script',files:['scripts/tests/test_input_artifact_reuse.mjs'],status:'added',reuse:true},
 {name:'migration note',files:['docs/libvirtualhid-experimental.md'],reuse:true},
 {name:'changed CMake flag',old:previous.replace('-DSUNSHINE_ENABLE_DRM=ON','-DSUNSHINE_ENABLE_DRM=OFF')},
 {name:'changed compiler image',old:previous.replace('image: archlinux:latest','image: archlinux:other')},
 {name:'changed dependency recipe',old:previous.replace('base-devel git cmake ninja boost','base-devel git cmake ninja boost OLD')},
 {name:'changed global env',old:previous.replace('permissions:','env:\n  CXXFLAGS: -O0\n\npermissions:')},
 {name:'unrecognized job format',now:current.replace(/^  linux:/m,"  'linux':")},
 {name:'cross-job compiler input',now:current.replace('    name: Linux input (libvirtualhid)',"    env:\n      FLAG: $"+"{{ needs.broker.outputs.flag }}\n    name: Linux input (libvirtualhid)")},
 {name:'aliased compiler input',now:current.replace('    name: Linux input (libvirtualhid)','    env: *shared\n    name: Linux input (libvirtualhid)')},
 {name:'source change',files:['src/process.cpp']},
 {name:'compiled test change',files:['tests/unit/platform/test_session_broker_client.cpp']},
 {name:'pin change',files:['cmake/compile_definitions/linux.cmake']},
 {name:'renamed file',status:'renamed'},
 {name:'unrelated ancestry',ancestor:'other'},
 {name:'diverged history',comparison:'diverged'},
 {name:'expired artifact',expired:true},
 {name:'manual fresh build',eventName:'workflow_dispatch'},
 {name:'pull request',ref:'refs/pull/1/merge'}
];
for(const test of cases){
 let output;
 const core={setOutput:(_,v)=>output=v,info:()=>{}};
 const context={repo:{owner:'owner',repo:'repo'},ref:test.ref??'refs/heads/feature',sha:'current',eventName:test.eventName??'push'};
 const github={rest:{actions:{listWorkflowRuns:async()=>({data:{workflow_runs:[{id:123,head_sha:'compiled'}]}}),listWorkflowRunArtifacts:async()=>({data:{artifacts:[{name:'libvirtualhid-linux-input-tests',expired:test.expired??false}]}})},repos:{
 compareCommits:async()=>({data:{status:test.comparison??'ahead',merge_base_commit:{sha:test.ancestor??'compiled'},files:(test.files??[workflow]).map(filename=>({filename,status:test.status??'modified'}))}}),
 getContent:async({ref})=>({data:{type:'file',encoding:'base64',content:Buffer.from(ref==='current'?(test.now??current):(test.old??previous)).toString('base64')}})
 }}};
 await script(core,context,github);
 if(output!==(test.reuse?'123':''))throw Error(test.name+': '+output);
}
process.stdout.write(`Artifact reuse: ${cases.length} validation cases passed\n`);
