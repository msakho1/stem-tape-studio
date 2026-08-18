import { defineConfig } from "vite";
export default defineConfig({
  root: __dirname,
  base: "./",
  publicDir: `${__dirname}/public`,
  build: { outDir: "/tmp/prepare-harness-dist", emptyOutDir: true, target: "es2022" },
});
