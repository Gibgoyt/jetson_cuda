vim.api.nvim_create_user_command('Term', function()
	local dir = ""
	if vim.bo.filetype == 'netrw' then
		dir = vim.b.netrw_curdir
	else
		dir = vim.fn.expand('%:p:h')
	end
	if dir and dir ~= "" then
		vim.cmd('split')
		vim.cmd('lcd ' .. dir)
		vim.cmd('terminal')
		vim.cmd('startinsert')
	else
		print("Error: Could not determine directory")
	end
end, {})

-- Bootstrap lazy.nvim
local lazypath = vim.fn.stdpath("data") .. "/lazy/lazy.nvim"
if not vim.loop.fs_stat(lazypath) then
  vim.fn.system({ "git", "clone", "--filter=blob:none",
    "https://github.com/folke/lazy.nvim.git", lazypath })
end
vim.opt.rtp:prepend(lazypath)

require("lazy").setup({
  { "hrsh7th/nvim-cmp" },
  { "hrsh7th/cmp-nvim-lsp" },
  -- nvim-lspconfig still needed as a source of default server configs,
  -- but we no longer call require('lspconfig').X.setup()
  { "neovim/nvim-lspconfig" },
})

-- Completion setup
local cmp = require("cmp")
cmp.setup({
  mapping = cmp.mapping.preset.insert({
    ["<Tab>"]     = cmp.mapping.select_next_item(),
    ["<S-Tab>"]   = cmp.mapping.select_prev_item(),
    ["<CR>"]      = cmp.mapping.confirm({ select = true }),
    ["<C-Space>"] = cmp.mapping.complete(),
  }),
  sources = {
    { name = "nvim_lsp" },
  },
})

-- New 0.11+ API: vim.lsp.config + vim.lsp.enable
vim.lsp.config('clangd', {
  cmd = { 'clangd' },
  filetypes = { 'c', 'cpp', 'objc', 'objcpp', 'cuda' },
  root_markers = { 'compile_commands.json', 'compile_flags.txt', '.git' },
  capabilities = require("cmp_nvim_lsp").default_capabilities(),
})

vim.lsp.enable('clangd')

-- LSP keymaps
vim.api.nvim_create_autocmd("LspAttach", {
  callback = function(ev)
    local opts = { buffer = ev.buf }
    vim.keymap.set("n", "gd",           vim.lsp.buf.definition,   opts)
    vim.keymap.set("n", "K",            vim.lsp.buf.hover,        opts)
    vim.keymap.set("n", "gr",           vim.lsp.buf.references,   opts)
    vim.keymap.set("n", "<leader>rn",   vim.lsp.buf.rename,       opts)
    vim.keymap.set("n", "<leader>ca",   vim.lsp.buf.code_action,  opts)
  end,
})

vim.diagnostic.config({
  virtual_text = true,   -- shows message to the right of the line
  signs = true,
  underline = true,
  update_in_insert = false,
})

-- Stop C/C++ smart-indent from yanking `#` lines (#define, #if, #ifdef, #endif,
-- #include) to column 0. With these removed, preprocessor directives keep the
-- surrounding indent like any other line.
vim.api.nvim_create_autocmd("FileType", {
  pattern = { "c", "cpp", "cuda", "objc", "objcpp" },
  callback = function()
    vim.opt_local.cinkeys:remove("0#")
    vim.opt_local.indentkeys:remove("0#")
    -- cinoptions #N: N>0 indents preprocessor lines by N shiftwidths from the
    -- base indent; the trailing 'C' (clang-format-compatible) keeps PP lines
    -- aligned with surrounding code rather than special-cased.
    vim.opt_local.cinoptions:append("#1")
  end,
})
