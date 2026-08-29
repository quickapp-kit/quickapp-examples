# media-001

Toolkit-generated Video contract baseline. The RPK carries the public Video props and handler semantics, one small local poster, and one local H.264 MP4. Player and decoder behavior remain platform responsibilities.

The video is referenced by the DSL as `assets/videos/demo.mp4`; it has no
network dependency. Android VideoView and iOS AVPlayer consume the packaged
resource through their platform adapters.
