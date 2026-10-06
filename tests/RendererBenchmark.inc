// Included in VulkanHarness: reuse its tested device setup and the production renderer.
// This is an offscreen integration workload, not a guest PS5 executable or game-FPS proxy.
void RunRendererBenchmark(const char* directory, uint32_t frames, uint32_t draws) {
  constexpr const char* name = "RendererBenchmark";
  constexpr uint32_t side = 512, warmup = 16, cubes = 64, vertices_per_cube = 36;
  constexpr uintptr_t base = 0x0000000207000000ull;
  constexpr uint64_t allocation_size = 0x800000;
  const auto file = [&](const char* leaf) {
    const auto path = std::string(directory) + "/" + leaf;
    auto* result = std::fopen(path.c_str(), "wb");
    Require(name, "output", result != nullptr, "could not open " + path);
    return result;
  };
  Require(name, "rasterization", RasterizationSupported(),
          "production rasterization features unavailable; benchmark cannot silently skip");
  const auto properties = m_physical_device.getProperties();
  const auto valid_bits = m_physical_device.getQueueFamilyProperties()[m_queue_family].timestampValidBits;
  Require(name, "GPU timestamps", valid_bits != 0 && properties.limits.timestampPeriod > 0,
          "graphics queue does not support timestamps");
  const uint64_t timestamp_mask = valid_bits == 64 ? UINT64_MAX : (uint64_t{1} << valid_bits) - 1;

  EnsureRuntimeContext();
  RenderContext context(m_runtime_context);
  auto& scheduler = context.GetCommandScheduler();
  auto& cache = context.GetTextureCache();
  auto& executor = context.GetRenderExecutor();
  HW::Context registers{};
  HW::UserConfig user_config{};
  HW::Shader shaders{};
  registers.SetRenderTargetMask(0xf);
  scheduler.Begin(registers, user_config, shaders);
  int64_t direct_offset = -1;
  Require(name, "allocate", Libs::LibKernel::Memory::KernelAllocateDirectMemory(
              0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(),
              allocation_size, allocation_size, 0, &direct_offset) == 0, "direct allocation failed");
  void* mapped = reinterpret_cast<void*>(base);
  Require(name, "map", Libs::LibKernel::Memory::KernelMapDirectMemory(
              &mapped, allocation_size, 0x3, 0x10, direct_offset, allocation_size) == 0 &&
              mapped == reinterpret_cast<void*>(base), "direct mapping failed");
  std::memset(mapped, 0, allocation_size);
  context.MapMemory(base, allocation_size);

  RenderColorInfo color{};
  color.desc.type = BindingType::RenderTarget;
  color.desc.info.data = {base, side * side * 16};
  color.desc.info.mip_layout[0] = {0, color.desc.info.data.size, side, side};
  color.desc.info.pixel_format = vk::Format::eR32G32B32A32Sfloat;
  color.desc.info.guest_format = Prospero::BufferFormat::k32_32_32_32Float;
  color.desc.info.extent = {side, side, 1};
  color.desc.info.pitch = side;
  color.desc.info.bytes_per_block = 16;
  color.desc.view_info.format = color.desc.info.pixel_format;
  color.desc.view_info.usage = vk::ImageUsageFlagBits::eColorAttachment;
  color.image_id = cache.FindImage(color.desc);
  RenderDepthInfo depth{};
  depth.desc.type = BindingType::DepthTarget;
  depth.desc.info.data = {base + 0x400000, side * side * 4};
  depth.desc.info.mip_layout[0] = {0, depth.desc.info.data.size, side, side};
  depth.desc.info.pixel_format = vk::Format::eD32Sfloat;
  depth.desc.info.guest_format = Prospero::BufferFormat::k32Float;
  depth.desc.info.extent = {side, side, 1};
  depth.desc.info.pitch = side;
  depth.desc.info.bytes_per_block = 4;
  depth.desc.info.tile_mode = Prospero::TileMode::kLinear;
  depth.desc.view_info.format = depth.desc.info.pixel_format;
  depth.desc.view_info.aspect = vk::ImageAspectFlagBits::eDepth;
  depth.desc.view_info.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;
  depth.depth_test_enable = depth.depth_write_enable = true;
  depth.depth_compare_op = vk::CompareOp::eLessOrEqual;
  depth.image_id = cache.FindImage(depth.desc);

  // Fixed 8x8 grid of rotated 3D cubes. Perspective is baked into homogeneous positions;
  // geometry generation and uploads are outside measured frames.
  constexpr std::array<std::array<float, 3>, 8> corners{{
      {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
      {-1,-1,1}, {1,-1,1}, {1,1,1}, {-1,1,1}}};
  constexpr std::array<uint32_t, 36> indices{
      0,2,1, 0,3,2, 4,5,6, 4,6,7, 0,1,5, 0,5,4,
      3,7,6, 3,6,2, 0,4,7, 0,7,3, 1,2,6, 1,6,5};
  std::vector<float> vertices;
  for (uint32_t cube = 0; cube < cubes; ++cube) {
    for (uint32_t i = 0; i < indices.size(); ++i) {
      const auto p = corners[indices[i]];
      const float x = .8660254f * p[0] + .5f * p[2];
      const float z = -.5f * p[0] + .8660254f * p[2];
      const float y = .8660254f * p[1] - .5f * z;
      const float camera_z = 6.f + .18f * (.5f * p[1] + .8660254f * z);
      const float shade = .45f + .09f * (i / 6);
      vertices.insert(vertices.end(), {
          1.6f * (.18f*x + (int(cube % 8) - 3.5f)*.58f),
          1.6f * (.18f*y + (int(cube / 8) - 3.5f)*.58f),
          (20.f / 19.9f)*camera_z - 2.f/19.9f, camera_z,
          shade, shade * (.3f + .08f*(cube % 8)), shade * .7f, 1.f});
    }
  }
  std::vector<u32> words(vertices.size());
  std::memcpy(words.data(), vertices.data(), vertices.size()*sizeof(float));
  auto buffer = CreateHostBuffer(name, words.size()*4, vk::BufferUsageFlagBits::eVertexBuffer, words);
  // Use the production download ring, not the test helper's first host-visible memory type.
  auto& readback = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
  const auto vs = TestSpv::MakePassthroughVertexSpirv(false, 1.f, true);
  ValidateSpirv(name, vs);
  GraphicsCase test;
  test.name = name;
  for (uint32_t component = 0; component < 4; ++component) {
    test.fragment_code.push_back(EncodeVintrp(0, component, 0, component, 0));
    test.fragment_code.push_back(EncodeVintrp(1, component, 0, component, 1));
  }
  test.fragment_code.push_back(EncodeExp0(0x00, 0xf));
  test.fragment_code.push_back(EncodeExp1(0, 1, 2, 3));
  AppendEnd(&test.fragment_code);
  auto fragment = CompileFragmentCase(test); // Real RDNA2 -> IR -> SPIR-V pixel compiler.
  ShaderProgram vertex_shader{0xb001, CreateShaderModule(name, vs)};
  ShaderProgram pixel_shader{0xb002, CreateShaderModule(name, fragment.spirv)};
  ShaderRecompiler::IR::CompiledShaderInfo vertex_program{};
  vertex_program.stage = ShaderType::Vertex;
  vertex_program.info.vertex_fetch_components[0] = 4;
  vertex_program.info.vertex_fetch_components[1] = 4;
  ShaderRecompiler::IR::ResourceSnapshot vertex_snapshot;
  ShaderVertexInputInfo vertex{};
  vertex.stage.program = &vertex_program;
  vertex.stage.resources = &vertex_snapshot;
  vertex.resources_num = 2;
  vertex.buffers_num = 1;
  vertex.buffers[0].stride = 8*sizeof(float);
  vertex.resources[1].UpdateAddress48(4*sizeof(float));
  for (uint32_t i = 0; i < 2; ++i) {
    vertex.resources[i].fields[3] = DstSel(4,5,6,7) |
        (static_cast<uint32_t>(Prospero::BufferFormat::k32_32_32_32Float) << 12u);
    vertex.resources_dst[i].registers_num = 4;
  }
  ShaderPixelInputInfo pixel{};
  ShaderRecompiler::IR::CompiledShaderInfo pixel_program{};
  pixel_program.stage = ShaderType::Pixel;
  pixel_program.info = fragment.program.info;
  pixel_program.bindings = fragment.program.bindings;
  pixel.stage.program = &pixel_program;
  pixel.stage.resources = &fragment.resources;
  const auto pipeline = [&]() -> PipelineCache::Pipeline& {
    return context.GetPipelineCache().GetGraphicsPipeline(
        std::span{&color, 1u}, depth, std::span{&vertex, 1u}, scheduler.Current(), &pixel,
        vk::PrimitiveTopology::eTriangleList, false,
        PipelineCache::GraphicsPrograms{{vertex_shader}, pixel_shader});
  };
  (void)pipeline(); // Compile/creation excluded from timing.
  vk::QueryPoolCreateInfo query_info{};
  query_info.queryType = vk::QueryType::eTimestamp;
  query_info.queryCount = 4;
  vk::QueryPool queries;
  RequireVk(name, "query pool", m_device.createQueryPool(&query_info, nullptr, &queries), "createQueryPool");

  const auto now = [] { return std::chrono::steady_clock::now(); };
  const auto ms = [&](auto begin) { return std::chrono::duration<double, std::milli>(now()-begin).count(); };
  const auto copy_pixels = [&]() {
    const auto [mapped_pixels, readback_offset] = readback.Map(side*side*16, 64);
    Require(name, "download ring", mapped_pixels != nullptr, "download ring reservation failed");
    readback.Commit();
    auto& command = scheduler.Current();
    auto cmd = command.Handle();
    cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, queries, 2);
    auto& image = cache.GetImage(color.image_id);
    image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {}, cmd);
    vk::BufferImageCopy copy{};
    copy.bufferOffset = readback_offset;
    copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    copy.imageExtent = {side, side, 1};
    cmd.copyImageToBuffer(image.backing.image, vk::ImageLayout::eTransferSrcOptimal, readback.Handle(), 1, &copy);
    vk::BufferMemoryBarrier barrier{};
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = readback.Handle();
    barrier.offset = readback_offset;
    barrier.size = side*side*16;
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                        {}, 0, nullptr, 1, &barrier, 0, nullptr);
    cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, queries, 3);
    scheduler.FlushAndWait();
    readback.Invalidate(readback_offset, side*side*16);
    std::vector<u32> result(side*side*4);
    std::memcpy(result.data(), mapped_pixels, side*side*16);
    return result;
  };
  uint64_t expected_hash = 0;
  nlohmann::json metadata{
      {"schema", 1}, {"workload_version", "cube-grid-v2"}, {"device", std::string(properties.deviceName.data())},
      {"vendor_id", properties.vendorID}, {"device_id", properties.deviceID}, {"driver_version", properties.driverVersion},
      {"timestamp_period_ns", properties.limits.timestampPeriod}, {"timestamp_valid_bits", valid_bits},
      {"width", side}, {"height", side}, {"cubes", cubes}, {"vertices_per_draw", vertices_per_cube},
      {"draws_per_frame", draws}, {"minimum_warmup_frames", warmup}, {"minimum_warmup_ms", 2000},
      {"measured_frames_per_profile", frames},
      {"vertex_shader", "native passthrough; CPU-preprojected 3D geometry"},
      {"fragment_shader", "RDNA2 compiled through Kyty"}, {"execution", "offscreen renderer integration; one frame in flight"}};
  metadata["readback"] = {{"staging", "production BufferCache Download utility ring"},
      {"coherent", readback.IsCoherent()}, {"guest_fault_dispatch", false}};
  auto* csv = file("frames.csv");
  std::fprintf(csv, "profile,frame,cpu_record_ms,cpu_submit_ms,cpu_wait_ms,cpu_readback_ms,host_frame_ms,gpu_span_ms,gpu_readback_ms,render_submits,readback_bytes\n");
  for (const auto profile : {"raster", "submits", "readback"}) {
    const bool many_submits = std::strcmp(profile, "submits") == 0;
    const bool download = std::strcmp(profile, "readback") == 0;
    std::vector<u32> pixels;
    const auto warmup_begin = now();
    uint32_t warmup_count = 0, measured = 0;
    while (measured < frames) {
      const bool warming = warmup_count < warmup || ms(warmup_begin) < 2000;
      const auto frame_begin = now();
      double record_ms = 0, submit_ms = 0, wait_ms = 0, read_ms = 0;
      uint32_t submits = 0;
      auto record_begin = now();
      auto cmd = scheduler.Current().Handle();
      cmd.resetQueryPool(queries, 0, 4);
      cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, queries, 0);
      for (uint32_t draw = 0; draw < draws; ++draw) {
        auto& command = scheduler.Current();
        const auto& selected = pipeline(); // Hot production cache lookup per draw.
        RenderExecutorTestAccess::BindRenderTarget(executor, color.image_id);
        RenderExecutorTestAccess::BindRenderTarget(executor, depth.image_id);
        auto bindings = RenderExecutorTestAccess::PrepareGraphicsBindings(executor, vertex.stage, pixel.stage, true);
        std::array<PreparedBindings*, 2> stages{&bindings.vertex[0], &*bindings.pixel};
        auto rendering = RenderExecutorTestAccess::AcquireRenderTargets(executor, command, &color, 1, depth, stages);
        RenderExecutorTestAccess::CommitBindings(executor, command, selected, bindings.vertex[0], *bindings.pixel);
        rendering.color_attachments[0].is_clear = draw == 0;
        rendering.depth_stencil_attachment.depth_clear = draw == 0;
        rendering.depth_stencil_attachment.clear_value[0] = std::bit_cast<u32>(1.f);
        command.BeginRendering(rendering);
        cmd = command.Handle();
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, selected.pipeline);
        const vk::Viewport viewport{0, 0, float(side), float(side), 0, 1};
        const vk::Rect2D scissor{{0,0},{side,side}};
        cmd.setViewportWithCount(1, &viewport);
        cmd.setScissorWithCount(1, &scissor);
        cmd.setLineWidth(1);
        cmd.setDepthTestEnable(true);
        cmd.setDepthWriteEnable(true);
        cmd.setDepthCompareOp(vk::CompareOp::eLessOrEqual);
        cmd.setDepthBiasEnable(false);
        cmd.setStencilTestEnable(false);
        const vk::Bool32 write = true;
        cmd.setColorWriteEnableEXT(1, &write);
        cmd.setAttachmentFeedbackLoopEnableEXT({});
        const vk::DeviceSize offset = 0;
        cmd.bindVertexBuffers(0, 1, &buffer.buffer, &offset);
        cmd.draw(vertices_per_cube, 1, (draw % cubes)*vertices_per_cube, 0);
        command.EndRendering();
        RenderExecutorTestAccess::ResetBindings(executor);
        if (many_submits && (draw+1) % 4 == 0 && draw+1 != draws) {
          record_ms += ms(record_begin);
          const auto begin = now();
          scheduler.Flush();
          submit_ms += ms(begin);
          ++submits;
          record_begin = now();
        }
      }
      scheduler.Current().Handle().writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, queries, 1);
      record_ms += ms(record_begin);
      auto begin = now();
      scheduler.Flush();
      submit_ms += ms(begin);
      ++submits;
      const auto tick = scheduler.CurrentTick()-1;
      begin = now();
      scheduler.Wait(tick);
      wait_ms = ms(begin);
      if (download) {
        begin = now();
        pixels = copy_pixels();
        read_ms = ms(begin);
      }
      const double host_ms = ms(frame_begin);
      std::array<uint64_t, 4> ticks{};
      RequireVk(name, "timestamps", m_device.getQueryPoolResults(queries, 0, download ? 4u : 2u,
          sizeof(ticks), ticks.data(), sizeof(uint64_t), vk::QueryResultFlagBits::e64), "getQueryPoolResults");
      const auto gpu_ms = [&](uint32_t first) {
        return double((ticks[first+1]-ticks[first]) & timestamp_mask) * properties.limits.timestampPeriod / 1e6;
      };
      if (!warming) {
        std::fprintf(csv, "%s,%u,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%u,%u\n",
            profile, measured++, record_ms, submit_ms, wait_ms, read_ms, host_ms,
            gpu_ms(0), download ? gpu_ms(2) : 0, submits, download ? side*side*16 : 0);
      } else {
        ++warmup_count;
      }
    }
    if (!download) { pixels = copy_pixels(); }
    uint64_t hash = 14695981039346656037ull;
    uint32_t covered = 0;
    for (size_t i = 0; i < pixels.size(); ++i) {
      hash = (hash ^ pixels[i]) * 1099511628211ull;
      if (i % 4 == 0 && std::bit_cast<float>(pixels[i]) > 0) { ++covered; }
    }
    Require(name, "coverage", covered > side*side/20 && covered < side*side*3/4,
            "3D grid did not render, or unexpectedly filled the image");
    if (expected_hash == 0) { expected_hash = hash; }
    Require(name, "image equivalence", hash == expected_hash, "submission/readback profile changed the rendered image");
    if (std::strcmp(profile, "raster") == 0) {
      auto* preview = file("scene.ppm");
      std::fprintf(preview, "P6\n%u %u\n255\n", side, side);
      for (size_t i = 0; i < pixels.size(); i += 4) {
        std::array<uint8_t, 3> rgb{};
        for (uint32_t channel = 0; channel < 3; ++channel) {
          const auto linear = std::clamp(std::bit_cast<float>(pixels[i+channel]), 0.f, 1.f);
          rgb[channel] = uint8_t(std::round(255.f * std::pow(linear, 1.f/2.2f)));
        }
        Require(name, "preview", std::fwrite(rgb.data(), 1, rgb.size(), preview) == rgb.size(), "preview write failed");
      }
      Require(name, "preview", std::fclose(preview) == 0, "preview close failed");
    }
    metadata["profiles"][profile] = {{"image_hash_u32_fnv1a64", std::to_string(hash)},
        {"covered_pixels", covered}, {"actual_warmup_frames", warmup_count}};
    std::printf("[benchmark] %s: %u measured frames, checksum=%" PRIu64 ", covered=%u\n", profile, frames, hash, covered);
  }
  Require(name, "output", std::fclose(csv) == 0, "could not flush frames.csv");
  auto* meta = file("workload.json");
  const auto text = metadata.dump(2) + "\n";
  Require(name, "metadata", std::fwrite(text.data(), 1, text.size(), meta) == text.size(), "metadata write failed");
  Require(name, "metadata", std::fclose(meta) == 0, "metadata close failed");
  scheduler.Finish();
  context.UnmapMemory(base, allocation_size);
  scheduler.Finish();
  m_device.destroyQueryPool(queries);
  DestroyBuffer(&buffer);
  m_device.destroyShaderModule(pixel_shader.module);
  m_device.destroyShaderModule(vertex_shader.module);
  Require(name, "release", Libs::LibKernel::Memory::KernelMunmap(base, allocation_size) == 0 &&
      Libs::LibKernel::Memory::KernelReleaseDirectMemory(direct_offset, allocation_size) == 0, "memory release failed");
}
