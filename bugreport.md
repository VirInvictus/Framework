### Bug Report: Immediate Fixes for Framework (GTK4/MuPDF)

This report identifies critical issues discovered during architectural review that compromise the application's performance, stability, and memory efficiency. These items require immediate attention to align with the project's goal of being a "fast, native GNOME document viewer with zero bloat."

#### **1. Critical: Memory Management & Leaks**

| Issue | Description | Impact |
| :--- | :--- | :--- |
| **MuPDF Global Cache Bloat** | `fz_new_context` uses `FZ_STORE_DEFAULT` (256MB). **Target: 64MB.** | Primary source of 200MB+ RAM overhead. 64MB is sufficient for 99% of documents. |
| **I/O & CPU Thrashing (Static Cache)** | `CACHE_WINDOW` is statically expanding to 50 pages regardless of user behavior. | Triggers massive I/O spikes and spins up CPU fans on document open or jump. Wastes cycles rendering pages the user is actively scrolling past. |
| **Peak Memory Spikes** | Simultaneous existence of `fz_pixmap` and `cairo_surface_t` during rendering copy. | Doubles memory requirements during every page render/re-render. |
| **Stale Surface Doubling** | `FwCache` retains old-generation surfaces during zoom/rotate until replaced. | Temporary 2x memory usage spikes (up to 100 surfaces) during every view change. |

#### **2. Structural: Thread Safety & Stability**

| Issue | Description | Impact |
| :--- | :--- | :--- |
| **Global Render Bottleneck** | Single `GMutex` serializes all MuPDF calls across background threads. | Rendering is single-threaded; stuttering during rapid scrolling on multi-core CPUs. |
| **DjVu Serialization Trap** | Rapid scrolling queues dozens of DjVu render jobs. `ddjvuapi` requires a mutex. | Locks the background thread at 100% CPU, creating a massive backlog and blocking the main thread from querying page states. |
| **Dangling Pointers in View** | `FwView` stores raw pointers to `document` and `cache` without ref-counting. | Switching documents clears objects but leaves view with invalid pointers. High crash risk. |
| **Improper Mutex Granularity** | Lock is held during `pixmap_to_cairo_surface` (non-MuPDF CPU-bound copy). | Blocks other threads from starting MuPDF jobs while a simple memory copy is in progress. |
| **Unsafe `fz_try` Variable Access** | Non-volatile variables (like `errmsg`) used inside `fz_try/fz_catch` blocks. | Potential for undefined behavior/crashes after a MuPDF exception (setjmp/longjmp rules). |

#### **3. Performance & IO Latency**

| Issue | Description | Impact |
| :--- | :--- | :--- |
| **Heavyweight Page Probing** | `pdf_open` iterates through all pages to pre-cache dimensions at startup. | **IO Bottleneck:** 1000+ MuPDF object lookups on open causes multi-second UI lag. Needs **Lazy Loading**. |
| **Inefficient DjVu Probing** | DjVu backend lacks a fast-path for dimensions; loads full page objects for every page on open. | Extreme startup latency and memory spike for large DjVu files. |
| **Outdated Documentation** | Internal comments estimate page sizes at 3-6MB (actual is 20MB+ at HiDPI). | Misleads developers regarding the actual memory footprint of the cache. |

### **Priority Action Items**

1.  ~~**Reduce MuPDF Store:** Initialize `fz_new_context` with a 64MB limit (`64 << 20`) to reclaim ~190MB RAM baseline.~~ *(Fixed in v1.2)*
2.  **Two-Tier Architecture:** Split the cache. Maintain a wider "Parsed Window" (cheap RAM, zero I/O) within the backend context, and a narrow, dynamic "Pixel Window" (cairo surfaces) managed by user velocity.
3.  ~~**Velocity-Driven Renders:** Use `gtk_widget_add_tick_callback` to track scroll `dy/dt`. Abort rendering entirely during high-velocity scrubbing. Only render ahead when cruising or static.~~ *(Fixed in v1.2)*
4.  ~~**Surgical Mutexing:** Move the Cairo surface copy *outside* the MuPDF global lock to enable better job pipelining and kill peak memory spikes.~~ *(Fixed in v1.2)*
5.  ~~**Thread Pool Pacing:** Force background render threads to yield and re-evaluate scroll velocity after every single page render. Stop dumping batches into the pool.~~ *(Fixed in v1.2)*
