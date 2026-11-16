// Included inside t2p, after the ordinary replay receiver. Group programs own
// immutable member descriptions; current poses live only in the recorded frame.
void Renderer3ds::learnDirectGroup(const std::shared_ptr<ReplayEntry>& entry,const Options& options){
 if(!directGroups_||!entry||entry->directGroupPublished||entry->groupCount<2||entry->groupCount>16||entry->groupCount>evs_.size()||
    !g_picaForceZDepth||drawRouted_||edit_||tweak_||skipDraw_||!entry->recipe||!entry->rec.ok||!entry->plan.ownedRows.empty()||entry->packet.second||entry->packet.pica().pass2)return;
 const size_t count=entry->groupCount;
 for(size_t i=0;i<count;++i){auto& use=evs_[i];if(use.kind!=Ev::Draw||!use.geo||use.geo->id!=entry->groupIds[i]||use.state().proj.ortho)return;directRegistry_->request(use.geo->id,use.geo);}
 for(size_t i=0;i<count;++i)if(!evs_[i].rawSignature)return;
 try{
  auto group=std::make_shared<DirectGroupBinding>();group->executor=entry;group->recipe=entry->recipe;
  std::vector<const UniformRecipe*> recipes;recipes.reserve(count);
  for(size_t i=0;i<count;++i){auto& use=evs_[i];auto binding=std::make_shared<DirectDrawBinding>(use.rawSignature);
   binding->geometryOwner=use.geo;binding->geometry=use.geo->id;binding->state=use.state();binding->groupRole=uint16_t(i);binding->group=group;binding->executor=entry;
   binding->state.draw.numPosMtx=std::clamp(use.geo->maxPm+1,1,10);binding->state.draw.vtx.numPosMtx=1;
   binding->compiled=drawCache_.get(binding->state.draw,options);
   if(!binding->compiled||!binding->compiled->vs.ok||binding->compiled->pass2||binding->compiled->overall==Fidelity::Unsupported)return;
   const auto& packet=entry->packet;std::memcpy(binding->projection,packet.frame.proj,sizeof binding->projection);std::memcpy(binding->post,packet.frame.post,sizeof binding->post);
   const auto view=use.state().uniformView();std::memcpy(binding->scale,view.texScale,sizeof binding->scale);
   for(int u=0;u<packet.pica().numUnits&&u<3;++u){const auto& unit=packet.units[u];if(unit.scratch||!unit.tex.valid||unit.texmap<0)return;binding->scale[u][0]=unit.scaleS;binding->scale[u][1]=unit.scaleT;}
   auto packed=std::make_unique<PackedUniforms>();packUniformsPacked(*packed,binding->compiled->vtx,binding->compiled->vs,view,packet.frame.proj,binding->scale);
   unsigned at=0;for(unsigned a=0;a<8;++a)if(packed->arrays[a].count){const auto n=unsigned(packed->arrays[a].count);binding->ranges.push_back({uint8_t(a),0,uint16_t(n),uint16_t(at)});at+=n;}
   auto recipe=std::make_shared<UniformRecipe>();if(!recipe->compile(binding->compiled->vtx,binding->compiled->vs,binding->ranges)||!rawRecipeValid(*recipe))return;
   binding->recipe=recipe;recipes.push_back(recipe.get());group->bytes+=sizeof(DirectDrawBinding)+4096+recipe->ops.size()*sizeof(UniformRecipe::Op);
   group->members.push_back(std::move(binding));
  }
  if(!group->rows.compile(*entry->recipe,recipes,entry->groupSlots,entry->groupPalette))return;
  group->bytes+=group->rows.copies.size()*sizeof(DirectGroupRows::Copy);
  if(directGroupRegistry_->publish(group))entry->directGroupPublished=true;
 }catch(const std::bad_alloc&){} // Required rendering always retains its ordinary fallback.
}

bool Renderer3ds::drawDirectGroup(const Options& options,int index,FrameReport& report,size_t& consumed){
 if(directGroupCold_||!directGroups_||evs_.empty()||evs_[0].kind!=Ev::DirectDraw)return false;
 const auto& first=evs_[0].stateStorage.payload();const auto group=first.groupOwner;
 if(!group||!first.owner||first.owner->groupRole||group->members.size()<2||group->members.size()>evs_.size())return false;
 const auto count=group->members.size();auto entry=std::static_pointer_cast<ReplayEntry>(group->executor.lock());
 if(!entry||entry->groupCount!=count||!entry->rec.ok||!entry->plan.ownedRows.empty()||!entry->plan.shader||reinterpret_cast<const void*>(entry->plan.shader->user)!=entry->rec.program)return false;
 auto resident=entry->residentLease.lock();if(!resident||resident->mem!=entry->residentMem)return false;
 std::array<std::span<const PVec>,16> input;
 for(size_t i=0;i<count;++i){const auto& use=evs_[i];if(use.kind!=Ev::DirectDraw||!use.geo)return false;const auto& payload=use.stateStorage.payload();const auto& member=group->members[i];
  if(payload.groupOwner!=group||payload.owner!=member||member->groupRole!=i||member->geometryOwner.lock()!=use.geo||member->geometry!=use.geo->id||
     payload.rowCount!=member->recipe->destinationRows||payload.firstRow>directRows_.size()||payload.rowCount>directRows_.size()-payload.firstRow)return false;
  // Preserve the original grouping's complete colour/projection comparisons,
  // including values an individual shader may not upload.
  if(std::memcmp(payload.guardColors,first.guardColors,sizeof first.guardColors)||std::memcmp(payload.projectionXY,first.projectionXY,sizeof first.projectionXY))return false;
  if(cache_.scaledRenderedTexmaps(member->state.tex)&~member->state.draw.texmapScaled)return false;
  input[i]={directRows_.data()+payload.firstRow,payload.rowCount};
 }
 std::array<PVec,96> rows;if(!group->rows.apply({input.data(),count},rows))return false;
 for(int u=0;u<entry->packet.pica().numUnits&&u<3;++u){const auto& unit=entry->packet.units[u];const auto* expected=entry->plan.units[u].entry;
  if(unit.scratch||!unit.tex.valid||unit.texmap<0)return false;
  if(!cache_.validThisFrame(expected)){TexMapOptions map;map.needAlpha=unit.needAlpha;auto got=cache_.get(unit.tex,*mem_,map);if(!got.entry||!got.error.empty()||got.mutableEntry()!=expected)return false;}
  if(!expected||!expected->user)return false;
 }
 if(resident->mem!=entry->residentMem)return false;
 const u64 start=drawTimingEnabled_?svcGetSystemTick():0;
 std::memcpy(entry->rows.get(),rows.data(),group->recipe->destinationRows*sizeof(PVec));resident->lastFrame=frames_;
 const uint32_t before=be_.recordedDraws();const bool drawn=be_.drawRecorded(entry->plan,entry->rec);
 report.replayRecorded+=be_.recordedDraws()-before;report.directDraws+=uint32_t(count);++report.directGroups;++report.replayGroupHits;++report.replayHits;++report.draws;
 if(drawn)++report.drawn;else ++report.refused;
 const double time=drawTimingEnabled_?msOf(svcGetSystemTick()-start):0;report.drawMs+=time;
 if(drawLog_){DrawReport d;d.index=index;d.packetOk=d.planOk=true;d.drawn=drawn;d.vertices=entry->packet.vertexCount;d.triangles=uint32_t(entry->packet.indexCount()/3);d.drawMs=time;static const std::vector<std::string> empty;d.packetNotes=d.planNotes=&empty;if(!drawn)d.why=be_.lastError();drawLog_(d);}
 entry->uniformBlock.reset();entry->uniformSections.reset();entry->drawnFrame=frames_;
 if(entry->availableFrame==frames_){entry->availableFrame=~0ull;replayCur_.insert(replayPrev_.extract(entry->location));}
 planPrevOwner_.reset();consumed=count;return true;
}
