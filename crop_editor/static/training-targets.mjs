function backendFor(outputBackends, sceneName) {
  if (!outputBackends || !sceneName) return "";
  if (typeof outputBackends.get === "function") return outputBackends.get(sceneName) || "";
  return outputBackends[sceneName] || "";
}

export function trainingOutputSceneName(sceneName, backend) {
  const name = String(sceneName || "").trim();
  if (backend !== "2dgs") return name;
  return name.toLowerCase().endsWith("_2dgs") ? name : `${name}_2dgs`;
}

export function existingTrainingOutputSceneName(sourceScene, requestedScene, backend, outputBackends = null) {
  const source = String(sourceScene || "").trim();
  const requested = trainingOutputSceneName(String(requestedScene || source).trim(), backend);
  if (!source || requested !== source) return requested;
  const existingBackend = backendFor(outputBackends, source);
  if (existingBackend && existingBackend !== backend) return `${source}_${backend}`;
  return requested;
}
