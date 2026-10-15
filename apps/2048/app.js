const SIZE=4,CELL=50,X=112,Y=5,EMPTY="#CDC1B4";
const COLORS={2:"#EEE4DA",4:"#EDE0C8",8:"#F2B179",16:"#F59563",32:"#F67C5F",64:"#F65E3B",128:"#EDCF72",256:"#EDCC61",512:"#EDC850",1024:"#EDC53F",2048:"#EDC22E"};
Pocket.setBackground("#FAF8EF");
Pocket.rect(108,1,208,208,"#BBADA0",10);
const grid=Pocket.grid(X,Y,4,4,CELL,4,EMPTY),labels=[];
for(let y=0;y<SIZE;y++)for(let x=0;x<SIZE;x++){const l=Pocket.text("",X+x*CELL,Y+16+y*CELL,CELL-4,"#776E65");Pocket.setTextAlign(l,"center");labels.push(l);}
Pocket.rect(4,12,98,54,"#BBADA0",10);
const scoreTitle=Pocket.text("SCORE",4,20,98,"#F9F6F2");Pocket.setTextAlign(scoreTitle,"center");
const scoreLabel=Pocket.text("0",4,41,98,"#FFFFFF");Pocket.setTextAlign(scoreLabel,"center");
let board=[],score=0,previousBoard=null,previousScore=0,won=false;
function blank(){board=[[0,0,0,0],[0,0,0,0],[0,0,0,0],[0,0,0,0]];}
function clone(source){return source.map(row=>row.slice());}
function addTile(){const empty=[];for(let y=0;y<SIZE;y++)for(let x=0;x<SIZE;x++)if(!board[y][x])empty.push([x,y]);if(!empty.length)return;const p=empty[Math.floor(Math.random()*empty.length)];board[p[1]][p[0]]=Math.random()<.9?2:4;}
function render(){for(let y=0;y<SIZE;y++)for(let x=0;x<SIZE;x++){const v=board[y][x],i=y*SIZE+x;Pocket.setGridCell(grid,x,y,v?(COLORS[v]||"#3C3A32"):EMPTY);Pocket.setText(labels[i],v?String(v):"");Pocket.setColor(labels[i],v>=8?"#F9F6F2":"#776E65");}Pocket.setText(scoreLabel,String(score));}
function slide(line){const values=line.filter(v=>v);let gained=0;for(let i=0;i+1<values.length;i++)if(values[i]===values[i+1]){values[i]*=2;gained+=values[i];values.splice(i+1,1);}while(values.length<SIZE)values.push(0);return[values,gained];}
function same(a,b){return a.join(",")===b.join(",");}
function move(direction){const before=clone(board),scoreBefore=score;let gained=0;for(let i=0;i<SIZE;i++){let line=[];for(let j=0;j<SIZE;j++)line.push(direction<2?board[i][j]:board[j][i]);if(direction===1||direction===3)line.reverse();const result=slide(line);line=result[0];gained+=result[1];if(direction===1||direction===3)line.reverse();for(let j=0;j<SIZE;j++)if(direction<2)board[i][j]=line[j];else board[j][i]=line[j];}let changed=false;for(let y=0;y<SIZE;y++)if(!same(before[y],board[y]))changed=true;if(!changed){Pocket.tone(180,20);return;}previousBoard=before;previousScore=scoreBefore;score+=gained;addTile();render();Pocket.tone(gained?760:420,18);if(!won&&board.some(row=>row.some(v=>v===2048))){won=true;Pocket.tone(1120,180);}}
function restart(){blank();score=0;won=false;previousBoard=null;addTile();addTile();render();}
function undo(){if(!previousBoard){Pocket.tone(180,20);return;}board=previousBoard;score=previousScore;previousBoard=null;render();Pocket.tone(520,25);}
function swipe(direction){if(direction==="left")move(0);else if(direction==="right")move(1);else if(direction==="up")move(2);else if(direction==="down")move(3);}
Pocket.button("NEW",4,78,98,38,restart,"#F59563");
Pocket.button("UNDO",4,124,98,38,undo,"#8F7A66");
const hint=Pocket.text("SWIPE\nTO MOVE",4,174,98,"#8F7A66");Pocket.setTextAlign(hint,"center");
Pocket.swipeArea(X,Y,200,200,swipe);
restart();
