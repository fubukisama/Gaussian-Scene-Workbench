# Gaussian Scene Workbench domain language

## Recovery workspace

A persistent working directory for an untitled project. It remains discoverable
after an abnormal exit and is deleted only after an explicit discard or after
its data has been adopted by a managed project.

## Recovery store

The deep module that owns recovery workspace identity, atomic recovery state,
project snapshots, retention, and safe completion or discard.

## Durable artifact

An immutable task output that has been copied from a stable source, hashed,
atomically published, and paired with an integrity record. Readers must reject
artifacts whose size or checksum does not match.

## Project snapshot

A versioned project-state manifest. Snapshots reuse managed immutable data and
durable artifacts rather than duplicating a complete scene.

## External backup store

A content-addressed second-storage location. Snapshot manifests reference
deduplicated objects; restore verifies every object and repairs managed paths
for externally linked datasets and scenes.
