// Included in renderer3ds.cpp inside namespace t2p. All records/resources remain
// owned by replay nodes; programs hold weak references and never prolong eviction.
void Renderer3ds::learnModelProgram(const std::shared_ptr<ReplayEntry>* entries,size_t count){
 for(size_t at=0;at<count;){
  if(!entries[at]||!entries[at]->recipe||!entries[at]->rec.ok){++at;continue;}
  size_t end=at+1;while(end<count&&entries[end]&&entries[end]->recipe&&entries[end]->rec.ok)++end;
  if(end-at>=2){ReplayProgram program;program.count=uint8_t(end-at);for(size_t i=at;i<end;++i)program.entries[i-at]=entries[i];
   bool unique=true;for(size_t i=at;i<end;++i)for(size_t j=at;j<i;++j)if(entries[i]==entries[j])unique=false;
   if(!unique){at=end;continue;}
   for(size_t first=at;first<end;){size_t stop=first+1;while(stop<end&&entries[stop]->rec.mat==entries[first]->rec.mat&&entries[stop]->rec.program==entries[first]->rec.program&&entries[stop]->rec.layout==entries[first]->rec.layout)++stop;
    for(size_t i=first;i<stop;++i)program.segmentEnd[i-at]=uint8_t(stop-at);first=stop;}
   const uint64_t id=entries[at]->geoId;if(!modelProgramsCache_.count(id)&&modelProgramsCache_.size()>=512)modelProgramsCache_.erase(modelProgramsCache_.begin());modelProgramsCache_[id]=std::move(program);}
  at=end;
 }
}

namespace {
bool programSameNonPose(const VtxSetup& setup,const VtxShader& shader,const VtxUniformView& a,const VtxUniformView& b){
 if(std::memcmp(a.proj,b.proj,sizeof a.proj)||std::memcmp(a.matColor,b.matColor,sizeof a.matColor)||std::memcmp(a.ambColor,b.ambColor,sizeof a.ambColor)||std::memcmp(a.texScale,b.texScale,sizeof a.texScale))return false;
 for(int i=0;i<8;++i)if(shader.lightSlot[i]>=0&&std::memcmp(&a.light[i],&b.light[i],sizeof a.light[i]))return false;
 for(size_t i=0;i<setup.texgen.size()&&i<3;++i)if(shader.texMtxSlot[i]>=0){const int m=(setup.texgen[i].mtx-30)/3;if(m<0||m>=10||std::memcmp(a.texMtx[m],b.texMtx[m],sizeof a.texMtx[m]))return false;}
 return true;
}
}
bool Renderer3ds::executeGroupProgram(int index,FrameReport& report,size_t& consumed){
 auto found=modelGroupPrograms_.find(evs_[0].geo->id);if(found==modelGroupPrograms_.end())return false;
 // Four palette topologies per first geometry: a shared slot stays shared only
 // when its current source matrices are still exactly equal.
 for(const auto& weak:found->second.entries){
  auto entry=weak.lock();if(!entry||(entry->availableFrame!=frames_&&entry->drawnFrame!=frames_)||!entry->recipe||!entry->rec.ok||entry->groupCount<2||entry->groupCount>evs_.size()||
   !entry->plan.shader||reinterpret_cast<const void*>(entry->plan.shader->user)!=entry->rec.program||!entry->plan.ownedRows.empty())continue;
  const size_t count=entry->groupCount;auto resident=entry->residentLease.lock();if(!resident||resident->mem!=entry->residentMem)continue;
  const u64 start=drawTimingEnabled_?svcGetSystemTick():0;const auto first=evs_[0].state().uniformView();
  static const float identity[3][4]={{1,0,0,0},{0,1,0,0},{0,0,1,0}};
  const float (*positions[10])[4];const float (*normals[10])[4];bool assigned[10]={};
  for(int k=0;k<10;++k){positions[k]=k<first.poseCount?first.pos[k]:identity;normals[k]=k<first.poseCount?first.nrm[k]:identity;}
  bool valid=entry->groupPalette<=10&&entry->groupSlots.size()==count&&entry->groupIds.size()==count;
  const size_t prefix=offsetof(GxState,curPosMtx)-offsetof(GxState,kcolor);
  for(size_t i=0;valid&&i<count;++i){auto& use=evs_[i];
   if(use.kind!=Ev::Draw||!use.geo||use.geo->id!=entry->groupIds[i]||use.state().proj.ortho){valid=false;break;}
   use.state().draw.texmapScaled|=cache_.scaledRenderedTexmaps(use.state().tex);
   if(entry->tail.size()<prefix||!replayEq(entry->tail.data(),replayTailBegin(use.state()),prefix)||!sameDrawKeyIgnoringPoseCount(entry->draw,use.state().draw)){valid=false;break;}
   const auto view=use.state().uniformView();
   if(i&&!programSameNonPose(entry->packet.pica().vtx,entry->packet.pica().vs,first,view)){valid=false;break;}
   for(int src=0;src<10;++src){const int slot=entry->groupSlots[i][src];if(slot<0)continue;
    if(src>=view.poseCount||slot>=int(entry->groupPalette)){valid=false;break;}
    if(assigned[slot]){if(std::memcmp(positions[slot],view.pos[src],48)||std::memcmp(normals[slot],view.nrm[src],48)){valid=false;break;}}
    else{positions[slot]=view.pos[src];normals[slot]=view.nrm[src];assigned[slot]=true;}
   }
  }
  for(unsigned slot=0;valid&&slot<entry->groupPalette;++slot)if(!assigned[slot])valid=false;
  if(!valid)continue;
  const auto& packet=entry->packet;
  for(int u=0;valid&&u<packet.pica().numUnits&&u<3;++u){const auto& source=packet.units[u];const auto* expected=entry->plan.units[u].entry;
   if(source.scratch||source.texmap<0||!source.tex.valid){valid=false;break;}
   if(!cache_.validThisFrame(expected)){TexMapOptions options;options.needAlpha=source.needAlpha;auto got=cache_.get(source.tex,*mem_,options);if(!got.entry||!got.error.empty()||got.mutableEntry()!=expected){valid=false;break;}}
   if(!expected->user)valid=false;
  }
  if(!valid||resident->mem!=entry->residentMem)continue;
  float scale[3][2];std::memcpy(scale,first.texScale,sizeof scale);for(int u=0;u<packet.pica().numUnits&&u<3;++u){scale[u][0]=packet.units[u].scaleS;scale[u][1]=packet.units[u].scaleT;}
  if(!entry->recipe->apply(entry->rows.get(),entry->recipe->destinationRows,first,packet.frame.proj,scale,positions,normals,10))continue;
  resident->lastFrame=frames_;const uint32_t before=be_.recordedDraws();const bool drawn=be_.drawRecorded(entry->plan,entry->rec);report.replayRecorded+=be_.recordedDraws()-before;
  ++report.draws;++report.replayHits;++report.replayGroupHits;++report.modelGroupPrograms;report.modelGroupMembers+=uint32_t(count);if(drawn)++report.drawn;else++report.refused;
  const u64 stop=drawTimingEnabled_?svcGetSystemTick():0;const double time=msOf(stop-start);report.drawMs+=time;
  if(drawLog_){DrawReport d;d.index=index;d.packetOk=d.planOk=true;d.drawn=drawn;d.vertices=packet.vertexCount;d.triangles=uint32_t(packet.indexCount()/3);d.drawMs=time;static const std::vector<std::string> none;d.packetNotes=&none;d.planNotes=&none;if(!drawn)d.why=be_.lastError();drawLog_(d);}
  entry->drawnFrame=frames_;if(entry->availableFrame==frames_){entry->availableFrame=~0ull;replayCur_.insert(replayPrev_.extract(entry->location));}
  // Palette rows were assembled from several source states, so no one source
  // uniform identity represents this plan. Force ordinary fallback to repack.
  entry->uniformBlock.reset();entry->uniformSections.reset();planPrevOwner_.reset();consumed=count;return true;
 }
 return false;
}
bool Renderer3ds::executeModelProgram(int index,FrameReport& report,size_t& consumed){
 if(!recordOn_||evs_.empty()||!evs_[0].geo)return false;
 if(opt_.matrixBatch&&executeGroupProgram(index,report,consumed))return true;
 auto found=modelProgramsCache_.find(evs_[0].geo->id);if(found==modelProgramsCache_.end()){++report.modelProgramReject[0];return false;}
 const auto& program=found->second;const size_t count=program.count;if(count<2||count>evs_.size())return false;
 const u64 start=drawTimingEnabled_?svcGetSystemTick():0;
 std::array<std::shared_ptr<ReplayEntry>,8> entries;Backend3ds::RecordedItem items[8];
 std::array<std::shared_ptr<ResidentEntry>,8> residents;
 // Resolve all immutable owners and static identities before any GPU draw.
 for(size_t i=0;i<count;++i){
  auto e=program.entries[i].lock();auto& use=evs_[i];
  if(!e||(e->availableFrame!=frames_&&e->drawnFrame!=frames_)||!e->recipe||!e->rec.ok||use.kind!=Ev::Draw||!use.geo||use.geo->id!=e->geoId||use.state().proj.ortho||e->groupCount||
     !e->plan.ownedRows.empty()||!e->plan.shader||reinterpret_cast<const void*>(e->plan.shader->user)!=e->rec.program){++report.modelProgramReject[1];return false;}
  auto resident=e->residentLease.lock();if(!resident||resident->mem!=e->residentMem){++report.modelProgramReject[2];return false;}
  use.state().draw.texmapScaled|=cache_.scaledRenderedTexmaps(use.state().tex);
  const size_t bytes=replayTailBytes(use.state());
  if(e->tail.size()!=bytes||!replayEq(e->tail.data(),replayTailBegin(use.state()),bytes)||
     !sameDrawKeyIgnoringPoseCount(e->draw,use.state().draw)||e->draw.numPosMtx!=use.state().draw.numPosMtx||e->draw.vtx.numPosMtx!=use.state().draw.vtx.numPosMtx||
     use.state().uniformView().poseCount<e->recipe->poseCount){++report.modelProgramReject[3];return false;}
  residents[i]=std::move(resident);entries[i]=std::move(e);items[i]={&entries[i]->plan,&entries[i]->rec};
 }
 // Every unique texture is verified once for the entire program. A rendered-copy
 // or stale cache entry refuses before execution and returns to ordinary replay.
 const TexEntry* checked[24];bool checkedAlpha[24];size_t checkedCount=0;
 for(size_t i=0;i<count;++i){auto& e=*entries[i];const auto& p=e.packet;
  for(int u=0;u<p.pica().numUnits&&u<3;++u){const auto& source=p.units[u];const auto* expected=e.plan.units[u].entry;
   if(source.scratch||source.texmap<0||!source.tex.valid)return false;
   bool seen=false;for(size_t j=0;j<checkedCount;++j)if(checked[j]==expected&&checkedAlpha[j]==source.needAlpha){seen=true;break;}if(seen)continue;
   if(!cache_.validThisFrame(expected)){TexMapOptions options;options.needAlpha=source.needAlpha;auto got=cache_.get(source.tex,*mem_,options);if(!got.entry||!got.error.empty()||got.mutableEntry()!=expected)return false;}
   if(!expected->user)return false; // keep uploads/linear-pressure changes on the ordinary path
   checked[checkedCount]=expected;checkedAlpha[checkedCount++]=source.needAlpha;
  }
 }
 for(size_t i=0;i<count;++i)if(residents[i]->mem!=entries[i]->residentMem){++report.modelProgramReject[4];return false;} // pressure may release a buffer
 // Compiled source mappings write directly into the cached plan. No replayTry,
 // draw-cache lookup, mapFrame, PackedUniforms scratch or plan build per member.
 for(size_t i=0;i<count;++i){auto& e=*entries[i];residents[i]->lastFrame=frames_;const auto view=evs_[i].state().uniformView();float scale[3][2];std::memcpy(scale,view.texScale,sizeof scale);
  for(int u=0;u<e.packet.pica().numUnits&&u<3;++u){scale[u][0]=e.packet.units[u].scaleS;scale[u][1]=e.packet.units[u].scaleT;}
  if(!e.recipe->apply(e.rows.get(),e.recipe->destinationRows,view,e.packet.frame.proj,scale))return false;
 }
 const uint32_t before=be_.recordedDraws();bool drawn[8];
 for(size_t at=0;at<count;){const size_t stop=program.segmentEnd[at];const int done=stop-at>=2?be_.drawRecordedBlock(items+at,stop-at):-1;
  if(done<0){drawn[at]=be_.drawRecorded(*items[at].plan,*items[at].record);++at;}
  else{for(size_t j=at;j<stop;++j)drawn[j]=j-at<size_t(done);at=stop;}}
 report.replayRecorded+=be_.recordedDraws()-before;report.modelProgramHits++;report.modelProgramDraws+=uint32_t(count);
 const u64 end=drawTimingEnabled_?svcGetSystemTick():0;const double time=msOf(end-start);report.drawMs+=time;
 for(size_t i=0;i<count;++i){auto& e=*entries[i];residents[i]->lastFrame=frames_;e.uniformBlock=evs_[i].state().uniformBlock;e.uniformSections=evs_[i].state().uniformSections;
  ++report.draws;++report.replayHits;if(drawn[i])++report.drawn;else++report.refused;
  if(drawLog_){DrawReport d;d.index=index+int(i);d.packetOk=d.planOk=true;d.drawn=drawn[i];d.vertices=e.packet.vertexCount;d.triangles=uint32_t(e.packet.indexCount()/3);d.drawMs=i?0:time;
   static const std::vector<std::string> none;d.packetNotes=&none;d.planNotes=&none;if(!drawn[i])d.why=be_.lastError();drawLog_(d);}
  e.drawnFrame=frames_;if(e.availableFrame==frames_){e.availableFrame=~0ull;replayCur_.insert(replayPrev_.extract(e.location));}
 }
 planPrevOwner_.reset();consumed=count;return true;
}
