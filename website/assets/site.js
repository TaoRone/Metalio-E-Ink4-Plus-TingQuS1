document.getElementById('fb-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const form = e.target;
  const status = document.getElementById('fb-status');
  const btn = form.querySelector('button');
  const data = {
    nickname: form.nickname.value.trim(),
    contact: form.contact.value.trim(),
    content: form.content.value.trim(),
  };
  if (!data.content) { status.textContent = '请填写反馈内容'; return; }
  btn.disabled = true;
  status.textContent = '提交中…';
  // AMH WAF 会拦截含未转义中文的 POST 体：将非 ASCII 转义为 \uXXXX（PHP json_decode 可还原）
  const payload = JSON.stringify(data).replace(/[\u0080-\uffff]/g,
    (ch) => '\\u' + ch.charCodeAt(0).toString(16).padStart(4, '0'));
  try {
    const r = await fetch('api/feedback.php', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: payload,
    });
    const j = await r.json();
    if (j.ok) {
      status.textContent = '已收到，感谢反馈！';
      form.reset();
    } else {
      status.textContent = '提交失败：' + (j.error || '未知错误');
    }
  } catch (err) {
    status.textContent = '网络错误，请稍后再试';
  } finally {
    btn.disabled = false;
  }
});
