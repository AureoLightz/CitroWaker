// Producer sends shader-consumed rows; the GPU owner validates resource leases
// before executing its recorded command program. No FIFO state/plan rediscovery.
void Renderer3ds::learnDirect(Ev& use,const std::shared_ptr<ReplayEntry>& entry){
 if(!directEvents_||!use.geo||use.maxPm>(directGroups_?9:0)||!g_picaForceZDepth||
    drawRouted_||edit_||tweak_||skipDraw_||rigidClip_||!recordOn_||!entry||entry->groupCount||
    !entry->recipe||!rawRecipeValid(*entry->recipe)||!entry->rec.ok||!entry->plan.ownedRows.empty()||
    use.state().proj.ortho||entry->packet.second||entry->packet.pica().pass2||entry->recipe->poseCount>(directGroups_?10u:1u))return;
 for(int u=0;u<entry->packet.pica().numUnits&&u<3;++u){const auto& unit=entry->packet.units[u];if(unit.scratch||!unit.tex.valid||unit.texmap<0)return;}
 // Geometry can have several material instances in the same frame. Bind only
 // the exact inserted/resolved entry, never another node with the same id.
 if(entry->geoId!=use.geo->id||entry->tail.size()!=replayTailBytes(use.state())||
    !replayEq(entry->tail.data(),replayTailBegin(use.state()),entry->tail.size())||
    !sameDrawKeyIgnoringPoseCount(entry->draw,use.state().draw))return;
 try{
  directRegistry_->request(use.geo->id,use.geo);if(!use.rawSignature||!directRegistry_->canLearn(use.geo->id))return;
  auto binding=std::make_shared<DirectDrawBinding>(use.rawSignature);
  binding->geometryOwner=use.geo;binding->geometry=use.geo->id;binding->state=use.state();
  binding->compiled=entry->packet.picaOwner().lock();binding->recipe=entry->recipe;binding->executor=entry;
  for(const auto& m:entry->rowMap)binding->ranges.push_back({m.array,m.first,m.count,m.at});
  std::memcpy(binding->post,entry->packet.frame.post,sizeof binding->post);
  std::memcpy(binding->projection,entry->packet.frame.proj,sizeof binding->projection);
  const auto view=use.state().uniformView();std::memcpy(binding->scale,view.texScale,sizeof binding->scale);
  for(int u=0;u<entry->packet.pica().numUnits&&u<3;++u){binding->scale[u][0]=entry->packet.units[u].scaleS;binding->scale[u][1]=entry->packet.units[u].scaleT;}
  directRegistry_->publish(std::move(binding));
 }catch(const std::bad_alloc&){} // Optional acceleration never consumes required game memory.
}
void Renderer3ds::drawDirect(Ev& use,const Options& options,int index,FrameReport& report){
 const auto payload=use.stateStorage.payload();const auto binding=payload.owner;
 if(!binding||!binding->recipe||payload.rowCount!=binding->recipe->destinationRows||
    payload.firstRow>directRows_.size()||payload.rowCount>directRows_.size()-payload.firstRow)
   throw std::logic_error("invalid direct frame row range");
 const PVec* rows=directRows_.data()+payload.firstRow;
 const u64 begin=drawTimingEnabled_?svcGetSystemTick():0;
 auto entry=std::static_pointer_cast<ReplayEntry>(binding->executor.lock());
 auto resident=entry?entry->residentLease.lock():nullptr;
 bool valid=entry&&resident&&use.geo&&binding->geometryOwner.lock()==use.geo&&use.geo->id==entry->geoId&&
   !entry->groupCount&&entry->rec.ok&&entry->plan.ownedRows.empty()&&entry->plan.shader&&
   reinterpret_cast<const void*>(entry->plan.shader->user)==entry->rec.program&&resident->mem==entry->residentMem;
 if(valid)for(int u=0;u<entry->packet.pica().numUnits&&u<3;++u){
   const auto& unit=entry->packet.units[u];const auto* expected=entry->plan.units[u].entry;
   TexMapOptions map;map.needAlpha=unit.needAlpha;
   auto got=cache_.get(unit.tex,*mem_,map);
   if(!got.entry||!got.error.empty()||got.mutableEntry()!=expected||!got.entry->user){valid=false;break;}
 }
 if(valid&&resident->mem==entry->residentMem){
   std::memcpy(entry->rows.get(),rows,sizeof(PVec)*payload.rowCount);resident->lastFrame=frames_;
   const uint32_t before=be_.recordedDraws();const bool drawn=be_.drawRecorded(entry->plan,entry->rec);
   ++report.directDraws;report.replayRecorded+=be_.recordedDraws()-before;++report.draws;++report.replayHits;
   if(drawn)++report.drawn;else ++report.refused;
   const double time=drawTimingEnabled_?msOf(svcGetSystemTick()-begin):0;report.drawMs+=time;
   if(drawLog_){DrawReport d;d.index=index;d.packetOk=d.planOk=true;d.drawn=drawn;d.vertices=entry->packet.vertexCount;d.triangles=uint32_t(entry->packet.indexCount()/3);d.drawMs=time;static const std::vector<std::string> empty;d.packetNotes=d.planNotes=&empty;if(!drawn)d.why=be_.lastError();drawLog_(d);}
   entry->uniformBlock.reset();entry->uniformSections.reset();entry->drawnFrame=frames_;
   if(entry->availableFrame==frames_){entry->availableFrame=~0ull;replayCur_.insert(replayPrev_.extract(entry->location));}
   planPrevOwner_.reset();return;
 }
 // Resource eviction or a later primitive changing the retained batch does not
 // lose the draw: construct a cold packet using its already-prepared shader rows.
 ++report.directFallbacks;auto packed=std::make_shared<PackedUniforms>();
 for(const auto& r:binding->ranges){if(r.array>=8||r.first+r.count>40||r.destination+r.count>payload.rowCount)throw std::logic_error("invalid direct fallback recipe");
   auto& array=packed->arrays[r.array];std::memcpy(array.rows+r.first,rows+r.destination,sizeof(PVec)*r.count);array.count=std::max(array.count,int(r.first+r.count));}
 use.legacy();use.state()=binding->state;use.rawSignature.reset();
 if(binding->signature->dynamicProjection){
  auto uniforms=std::make_shared<VtxUniforms>(use.state().vertexUniforms());
  auto& proj=use.state().proj;
  for(int i=0;i<4;++i)std::memcpy(&proj.p[i],payload.projectionXY+i,4);
  proj.m[0][0]=proj.p[0];proj.m[0][2]=proj.p[1];proj.m[1][1]=proj.p[2];proj.m[1][2]=proj.p[3];
  std::memcpy(uniforms->proj,proj.m,sizeof proj.m);use.state().uniformBlock=std::move(uniforms);use.state().uniformSections.reset();
 }
 drawOne(use,options,index,report,nullptr,nullptr,binding->compiled,std::move(packed));
}
