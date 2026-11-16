export function analysisFooterState(progress,result,started,now) {
 const running=progress&&['running','cancelling'].includes(progress.state)&&result?.jobId!==progress.jobId;
 const percent=Math.floor(100*Math.max(0,Math.min(1,Number(progress?.progress)||0)));
 return {visible:!!running&&now-started>=1000,text:`Energy map ${percent} %${progress?.state==='cancelling'?' - cancelling':''}`};
}
