# Media

Photographs and video for the build write-up.

- `photos/` — build and bring-up photos, board shots, the assembled box.
- `video/` — footage and exports for the video.

## Read this before committing video

Git stores every version of every file forever. Deleting a large file in a later
commit does not shrink the repository: the blob stays in the history, and
rewriting history afterwards is disruptive for anyone who has already cloned it.
Two hard numbers from GitHub:

- **100 MB** is a hard per-file limit. A push containing a larger file is
  rejected outright.
- **1 GB** is where repository-size warnings start.

So raw footage does not belong in normal git. The options, in the order usually
worth trying:

1. **Keep it out of the repo.** Put the edit somewhere built for it — the video
   platform itself — and commit only a link and the stills. For a project whose
   point is the firmware, this is almost always the right answer.
2. **Git LFS** for anything large you genuinely want versioned, set up *before*
   the first commit of those files. `git lfs track "media/video/**"` writes a
   `.gitattributes` entry; files committed before LFS is configured are not
   retroactively moved.
3. **Compress hard.** A 1080p H.264 export at a sane bitrate is a few tens of MB
   and will go in directly, unlike the ProRes master.

Stills are rarely a problem: resize to something sensible for a README rather
than committing 8 MB straight off the camera.
